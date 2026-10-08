/* TM5 for Linux - data pattern generators.
 * SPDX-License-Identifier: MIT
 *
 * Every pattern is a pure function of the global cache-line index, so the expected value of any
 * line can be regenerated during verification no matter in which order memory is walked.
 */
#ifndef TM5L_PATTERN_H
#define TM5L_PATTERN_H

#include "tm5.h"

typedef enum {
    PK_SOLID,       /* the same word everywhere */
    PK_LINE_ALT,    /* a / ~a on alternating cache lines */
    PK_OWN_ADDR,    /* every word holds its own address */
    PK_WALK,        /* a single bit (or hole) walking through the word */
    PK_RANDOM,      /* counter-based pseudo random data */
    PK_ARITH,       /* start + index * step (TM5 pattern mode 2) */
} pattern_kind_t;

typedef struct {
    pattern_kind_t kind;
    uint64_t       a, b;
    uint64_t       inv;     /* 0 or ~0: odd passes write the inverse of even passes */
    const char    *name;
} pattern_t;

void pattern_select(const tm5_test_t *t, int test_index, int cycle, int pass, uint64_t run_seed, pattern_t *out);
void pattern_gen(const pattern_t *p, uint64_t gline, int64_t step, size_t n, uint64_t *restrict out);

#endif
