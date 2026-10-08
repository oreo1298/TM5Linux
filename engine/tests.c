/* TM5 for Linux - the memory test functions.
 * SPDX-License-Identifier: MIT
 *
 * TM5 itself is closed source, so these are independent implementations of the test functions a
 * TM5 config can ask for. They honour the same knobs (pattern mode, Param0/Param1, Parameter,
 * test block size, time) - see docs/TESTS.md for exactly how each one is interpreted.
 *
 * Memory is handled in 64-byte cache lines. Each worker thread owns one region and only ever
 * touches that region, so threads never share data and every mismatch is a real memory error.
 */
#include "tests.h"
#include "arch.h"
#include "pattern.h"
#include "util.h"

#include <time.h>

static inline bool stopping(void)
{
    return atomic_load_explicit(&g_stop, memory_order_relaxed) != 0;
}

static inline void progress(worker_t *w, uint64_t bytes)
{
    atomic_fetch_add_explicit(&w->done, bytes, memory_order_relaxed);
}

static inline size_t line_at(size_t first, size_t i, int64_t step)
{
    return (size_t)((int64_t)first + (int64_t)i * step);
}

static size_t block_bytes(const tm5_test_t *t, size_t region)
{
    size_t b = t->block_mb ? (size_t)t->block_mb << 20 : region;
    if (b > region || b < 2 * LINE_BYTES)
        b = region;
    return b;
}

/* ---- verification ------------------------------------------------------------------------ */

static void __attribute__((noinline))
mismatch_line(worker_t *w, const step_t *s, uint64_t *mem, const uint64_t *got, const uint64_t *exp, bool repair)
{
    for (unsigned k = 0; k < LINE_WORDS; k++) {
        if (got[k] == exp[k])
            continue;
        report_mismatch(w, s, &mem[k], exp[k], got[k]);
        if (repair) {
            ((volatile uint64_t *)mem)[k] = exp[k];
            flush_line(&mem[k]);
        }
    }
}

/* Compare one line with the expected data. The values are read exactly once and those same values
 * are reported, so short-lived read errors are not lost by re-reading. */
static inline void check_line(worker_t *w, const step_t *s, uint64_t *mem, const uint64_t *exp, bool repair)
{
    uint64_t got[LINE_WORDS], acc = 0;
    for (unsigned k = 0; k < LINE_WORDS; k++) {
        got[k] = mem[k];
        acc |= got[k] ^ exp[k];
    }
    if (__builtin_expect(acc != 0, 0))
        mismatch_line(w, s, mem, got, exp, repair);
}

/* Self-test only: flip one random bit somewhere in [blk, blk + nlines) to prove detection works. */
static void inject(worker_t *w, uint8_t *blk, size_t nlines)
{
    if (!g_inject || nlines == 0)
        return;
    uint64_t r = rng_next(&w->rng);
    if (g_inject_rate > 1 && (r >> 48) % g_inject_rate != 0)
        return;
    uint64_t *word = (uint64_t *)(blk + (r % nlines) * LINE_BYTES) + ((r >> 32) % LINE_WORDS);
    *(volatile uint64_t *)word ^= 1ULL << ((r >> 40) & 63);
    flush_line(word);
    atomic_fetch_add(&g_injected, 1);
}

/* ---- generic line walks ---------------------------------------------------------------------- */

/* Write (or verify) the lines first, first+step, first+2*step ... (n lines) of a block. */
static void walk_chain(worker_t *w, const step_t *s, const pattern_t *pat, uint8_t *blk, uint64_t gblk,
                       size_t first, int64_t step, size_t n, bool verify)
{
    for (size_t k = 0; k < n; k += BATCH_LINES) {
        size_t cnt = n - k < BATCH_LINES ? n - k : BATCH_LINES;
        size_t l0 = line_at(first, k, step);
        pattern_gen(pat, gblk + l0, step, cnt, w->buf);
        if (verify) {
            for (size_t i = 0; i < cnt; i++)
                check_line(w, s, (uint64_t *)(blk + line_at(l0, i, step) * LINE_BYTES), w->buf + i * LINE_WORDS, false);
        } else {
            for (size_t i = 0; i < cnt; i++)
                nt_line(blk + line_at(l0, i, step) * LINE_BYTES, w->buf + i * LINE_WORDS);
        }
        progress(w, cnt * LINE_BYTES);
        if (stopping())
            return;
    }
}

/* Visit every line of a block once. With stride S the block is walked as S interleaved chains
 * (0, S, 2S ... then 1, S+1 ...), so consecutive accesses land S*64 bytes apart. */
static void walk_block(worker_t *w, const step_t *s, const pattern_t *pat, uint8_t *blk, uint64_t gblk,
                       size_t nlines, size_t stride, bool reverse, bool verify)
{
    if (stride < 1 || stride >= nlines)
        stride = 1;
    for (size_t c = 0; c < stride && !stopping(); c++) {
        size_t start = reverse ? stride - 1 - c : c;
        size_t n = (nlines - 1 - start) / stride + 1;
        if (!reverse)
            walk_chain(w, s, pat, blk, gblk, start, (int64_t)stride, n, verify);
        else
            walk_chain(w, s, pat, blk, gblk, start + (n - 1) * stride, -(int64_t)stride, n, verify);
    }
}

/* ---- SimpleTest ---------------------------------------------------------------------------- */

static void run_simple(worker_t *w, const step_t *s)
{
    const tm5_test_t *t = s->t;
    const size_t bsz = block_bytes(t, w->bytes);
    const size_t stride = t->parameter > 1 ? (size_t)t->parameter : 1;

    for (int p = 0; p < s->passes && !stopping(); p++) {
        pattern_t pat;
        pattern_select(t, s->index, s->cycle, p, s->seed, &pat);
        const bool reverse = p & 1;     /* odd passes walk downwards */
        for (size_t off = 0; off < w->bytes && !stopping(); off += bsz) {
            size_t len = w->bytes - off < bsz ? w->bytes - off : bsz;
            size_t nlines = len / LINE_BYTES;
            uint8_t *blk = w->base + off;
            uint64_t gblk = w->gline0 + off / LINE_BYTES;

            walk_block(w, s, &pat, blk, gblk, nlines, stride, reverse, false);
            store_fence();
            inject(w, blk, nlines);
            if (stopping())
                return;
            walk_block(w, s, &pat, blk, gblk, nlines, stride, reverse, true);
        }
    }
}

/* ---- RefreshStable ------------------------------------------------------------------------- */

static void idle_wait(worker_t *w, uint64_t credit, unsigned ms)
{
    uint64_t given = 0;
    double t0 = now_sec();
    for (;;) {
        double el = (now_sec() - t0) * 1000.0;
        uint64_t want = el >= ms ? credit : (uint64_t)((double)credit * el / ms);
        if (want > given) {
            progress(w, want - given);
            given = want;
        }
        if (el >= ms || stopping())
            return;
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
    }
}

static void run_refresh(worker_t *w, const step_t *s)
{
    const size_t nlines = w->bytes / LINE_BYTES;
    for (int p = 0; p < s->passes && !stopping(); p++) {
        pattern_t pat;
        pattern_select(s->t, s->index, s->cycle, p, s->seed, &pat);
        walk_block(w, s, &pat, w->base, w->gline0, nlines, 1, false, false);
        store_fence();
        inject(w, w->base, nlines);
        idle_wait(w, w->bytes, s->refresh_idle_ms);     /* nobody touches the memory now */
        if (stopping())
            return;
        walk_block(w, s, &pat, w->base, w->gline0, nlines, 1, false, true);
    }
}

/* ---- MirrorMove / MirrorMove128 ------------------------------------------------------------ */

typedef struct {
    size_t half;    /* lines per half */
    size_t chunk;   /* lines per mirrored chunk */
    size_t full;    /* lines covered by whole chunks */
} mirror_t;

/* Chunk c of a half goes to chunk (n-1-c) of the other half. Applying it twice is the identity. */
static inline size_t mirror_of(const mirror_t *m, size_t i)
{
    if (i >= m->full)
        return i;
    size_t c = i / m->chunk;
    return (m->full / m->chunk - 1 - c) * m->chunk + (i - c * m->chunk);
}

/* What a line looks like after one mirrored copy: lanes reversed and every bit inverted. */
static inline void mirror_xform(const uint64_t *in, uint64_t *out, bool wide)
{
    if (wide) {
        for (unsigned l = 0; l < 4; l++) {
            out[2 * l] = ~in[2 * (3 - l)];
            out[2 * l + 1] = ~in[2 * (3 - l) + 1];
        }
    } else {
        for (unsigned k = 0; k < LINE_WORDS; k++)
            out[k] = ~in[LINE_WORDS - 1 - k];
    }
}

static void mirror_copy(worker_t *w, const uint8_t *src, uint8_t *dst, const mirror_t *m, bool wide)
{
    for (size_t i = 0; i < m->half; i += BATCH_LINES) {
        size_t cnt = m->half - i < BATCH_LINES ? m->half - i : BATCH_LINES;
        for (size_t k = i; k < i + cnt; k++) {
            uint8_t *d = dst + mirror_of(m, k) * LINE_BYTES;
            if (wide)
                copy_line_rev128_inv(d, src + k * LINE_BYTES);
            else
                copy_line_rev64_inv(d, src + k * LINE_BYTES);
        }
        progress(w, 2 * cnt * LINE_BYTES);      /* one read + one write per line */
        if (stopping())
            return;
    }
}

/* Check the mirrored copy against freshly generated data. Bad words are repaired so that a single
 * fault is reported once and not copied back into the other half. */
static void mirror_verify(worker_t *w, const step_t *s, const pattern_t *pat, uint8_t *dst, uint64_t gsrc,
                          const mirror_t *m, bool wide)
{
    for (size_t i = 0; i < m->half; i += BATCH_LINES) {
        size_t cnt = m->half - i < BATCH_LINES ? m->half - i : BATCH_LINES;
        pattern_gen(pat, gsrc + i, 1, cnt, w->buf);
        for (size_t k = 0; k < cnt; k++) {
            mirror_xform(w->buf + k * LINE_WORDS, w->xbuf + k * LINE_WORDS, wide);
            check_line(w, s, (uint64_t *)(dst + mirror_of(m, i + k) * LINE_BYTES), w->xbuf + k * LINE_WORDS, true);
        }
        progress(w, cnt * LINE_BYTES);
        if (stopping())
            return;
    }
}

static void run_mirror(worker_t *w, const step_t *s, bool wide)
{
    const tm5_test_t *t = s->t;
    const size_t bsz = block_bytes(t, w->bytes);

    for (int p = 0; p < s->passes && !stopping(); p++) {
        pattern_t pat;
        tm5_test_t tt = *t;
        if (tt.pattern_mode == 0)
            tt.pattern_mode = 1;        /* random data makes every copied line unique */
        pattern_select(&tt, s->index, s->cycle, p, s->seed, &pat);

        for (size_t off = 0; off < w->bytes && !stopping(); off += bsz) {
            size_t len = w->bytes - off < bsz ? w->bytes - off : bsz;
            size_t half = len / LINE_BYTES / 2;
            if (half == 0)
                continue;
            uint8_t *a = w->base + off, *b = a + half * LINE_BYTES;
            uint64_t ga = w->gline0 + off / LINE_BYTES;
            size_t chunk = t->parameter >= 1 ? (size_t)t->parameter : 1;
            if (chunk > half)
                chunk = half;
            mirror_t m = { half, chunk, half / chunk * chunk };

            walk_chain(w, s, &pat, a, ga, 0, 1, half, false);     /* 1. fill A          */
            store_fence();
            if (stopping())
                return;
            mirror_copy(w, a, b, &m, wide);                        /* 2. A -> B mirrored */
            store_fence();
            inject(w, b, half);
            if (stopping())
                return;
            mirror_verify(w, s, &pat, b, ga, &m, wide);            /* 3. check B         */
            store_fence();
            if (stopping())
                return;
            mirror_copy(w, b, a, &m, wide);                        /* 4. B -> A mirrored */
            store_fence();
            inject(w, a, half);
            if (stopping())
                return;
            walk_chain(w, s, &pat, a, ga, 0, 1, half, true);      /* 5. check A         */
        }
    }
}

/* ---- dispatch -------------------------------------------------------------------------------- */

uint64_t test_step_bytes(const tm5_test_t *t, size_t region, int passes, unsigned refresh_idle_ms)
{
    (void)refresh_idle_ms;
    uint64_t per_pass = 0;
    switch (t->func) {
    case FN_SIMPLE:
        per_pass = 2 * (uint64_t)region;
        break;
    case FN_REFRESH:
        per_pass = 3 * (uint64_t)region;        /* write + idle (counted as one region) + read */
        break;
    case FN_MIRROR:
    case FN_MIRROR128: {
        size_t bsz = block_bytes(t, region);
        for (size_t off = 0; off < region; off += bsz) {
            size_t len = region - off < bsz ? region - off : bsz;
            per_pass += 7 * (uint64_t)(len / LINE_BYTES / 2) * LINE_BYTES;
        }
        break;
    }
    }
    return per_pass * (uint64_t)passes;
}

void test_run(worker_t *w, const step_t *s)
{
    switch (s->t->func) {
    case FN_SIMPLE:    run_simple(w, s); break;
    case FN_REFRESH:   run_refresh(w, s); break;
    case FN_MIRROR:    run_mirror(w, s, false); break;
    case FN_MIRROR128: run_mirror(w, s, true); break;
    }
    store_fence();
}
