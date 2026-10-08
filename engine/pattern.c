/* TM5 for Linux - data pattern generators.
 * SPDX-License-Identifier: MIT
 */
#include "pattern.h"
#include "util.h"

#define GOLDEN 0x9e3779b97f4a7c15ULL

static inline uint64_t rotl64(uint64_t x, unsigned r)
{
    r &= 63;
    return r ? (x << r) | (x >> (64 - r)) : x;
}

static inline uint64_t rep32(uint64_t v)
{
    v &= 0xffffffffULL;
    return (v << 32) | v;
}

typedef struct {
    pattern_kind_t kind;
    uint64_t       a;
    const char    *name;
} classic_t;

/* Pattern mode 0 rotates through these classic memory-test patterns. Each one is also written
 * inverted on the following pass, so every bit is tested in both states. */
static const classic_t classic[] = {
    { PK_OWN_ADDR, 0,                     "own address" },
    { PK_SOLID,    0x0000000000000000ULL, "all zeros / all ones" },
    { PK_SOLID,    0x5555555555555555ULL, "checkerboard 0101" },
    { PK_LINE_ALT, 0x0000000000000000ULL, "alternating lines 00/FF" },
    { PK_WALK,     0x0000000000000001ULL, "walking one" },
    { PK_SOLID,    0x00ff00ff00ff00ffULL, "byte stripes" },
    { PK_RANDOM,   0,                     "random" },
    { PK_LINE_ALT, 0x3333333333333333ULL, "alternating lines 0011/1100" },
    { PK_SOLID,    0x0000ffff0000ffffULL, "word stripes" },
    { PK_LINE_ALT, 0x5555555555555555ULL, "alternating checkerboard" },
    { PK_WALK,     0x8000000000000001ULL, "walking pair" },
    { PK_SOLID,    0x6db6db6db6db6db6ULL, "3-bit stripes" },
};
#define NCLASSIC (sizeof classic / sizeof classic[0])

void pattern_select(const tm5_test_t *t, int test_index, int cycle, int pass, uint64_t run_seed, pattern_t *out)
{
    uint64_t round = (uint64_t)(pass / 2);
    out->inv = (pass & 1) ? ~0ULL : 0;
    out->b = 0;

    switch (t->pattern_mode) {
    case 0: {
        /* Offset by test number and cycle so different tests and cycles use different patterns. */
        size_t idx = (size_t)(round + (uint64_t)test_index * 5 + (uint64_t)(cycle - 1) * 7) % NCLASSIC;
        out->kind = classic[idx].kind;
        out->a = classic[idx].a;
        out->name = classic[idx].name;
        if (out->kind == PK_OWN_ADDR || out->kind == PK_RANDOM)
            out->a = mix64(run_seed ^ (round * GOLDEN) ^ ((uint64_t)test_index << 40) ^ ((uint64_t)cycle << 52));
        if (out->kind == PK_WALK)
            out->b = round + (uint64_t)cycle * 13;
        break;
    }
    case 1:
        out->kind = PK_RANDOM;
        out->a = mix64(run_seed ^ mix64(t->param0 * GOLDEN + t->param1) ^ (round * GOLDEN) ^
                       ((uint64_t)test_index << 40) ^ ((uint64_t)cycle << 52));
        out->name = "random";
        break;
    default:
        /* Mode 2: arithmetic sequence. Param1 is the start value, Param0 the step; both are
         * mirrored into the upper 32 bits so that all 64 data lines toggle. */
        out->kind = PK_ARITH;
        out->a = rep32(t->param1) ^ (round * GOLDEN) ^ ((uint64_t)(cycle - 1) * 0xd1b54a32d192ed03ULL);
        out->b = (t->param0 ? rep32(t->param0) : GOLDEN) | 1;
        out->name = "seeded sequence";
        break;
    }
}

void pattern_gen(const pattern_t *p, uint64_t gline, int64_t step, size_t n, uint64_t *restrict out)
{
    const uint64_t inv = p->inv;
    uint64_t line = gline;

    switch (p->kind) {
    case PK_SOLID: {
        const uint64_t v = p->a ^ inv;
        for (size_t i = 0; i < n * LINE_WORDS; i++)
            out[i] = v;
        break;
    }
    case PK_LINE_ALT:
        for (size_t i = 0; i < n; i++, line += (uint64_t)step) {
            const uint64_t v = ((line & 1) ? ~p->a : p->a) ^ inv;
            for (unsigned k = 0; k < LINE_WORDS; k++)
                out[i * LINE_WORDS + k] = v;
        }
        break;
    case PK_OWN_ADDR:
        for (size_t i = 0; i < n; i++, line += (uint64_t)step) {
            const uint64_t base = line * LINE_BYTES;
            for (unsigned k = 0; k < LINE_WORDS; k++)
                out[i * LINE_WORDS + k] = (base + k * 8) ^ p->a ^ inv;
        }
        break;
    case PK_WALK:
        for (size_t i = 0; i < n; i++, line += (uint64_t)step) {
            const uint64_t w = line * LINE_WORDS + p->b;
            for (unsigned k = 0; k < LINE_WORDS; k++)
                out[i * LINE_WORDS + k] = rotl64(p->a, (unsigned)(w + k)) ^ inv;
        }
        break;
    case PK_RANDOM:
        for (size_t i = 0; i < n; i++, line += (uint64_t)step) {
            const uint64_t h = mix64(p->a ^ (line * GOLDEN));
            for (unsigned k = 0; k < LINE_WORDS; k++) {
                uint64_t x = h + (uint64_t)k * GOLDEN;
                x = (x ^ (x >> 32)) * 0xd6e8feb86659fd93ULL;
                out[i * LINE_WORDS + k] = (x ^ (x >> 32)) ^ inv;
            }
        }
        break;
    case PK_ARITH:
        for (size_t i = 0; i < n; i++, line += (uint64_t)step) {
            const uint64_t w = line * LINE_WORDS;
            for (unsigned k = 0; k < LINE_WORDS; k++)
                out[i * LINE_WORDS + k] = (p->a + (w + k) * p->b) ^ inv;
        }
        break;
    }
}
