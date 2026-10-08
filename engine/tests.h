/* TM5 for Linux - the memory test functions.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_TESTS_H
#define TM5L_TESTS_H

#include "tm5.h"

typedef struct {
    int               index;            /* TestN number in the config */
    const tm5_test_t *t;
    int               cycle;            /* 1-based */
    int               step;             /* 0-based position in "Test Sequence" */
    int               passes;
    uint64_t          seed;
    unsigned          refresh_idle_ms;
} step_t;

typedef struct {
    _Alignas(64) _Atomic uint64_t done; /* bytes processed in the current step */
    uint64_t         total;             /* bytes the current step will process */
    _Atomic uint64_t errors;            /* errors found by this thread during the run */
    int              id;
    int              cpu;               /* pinned CPU, -1 if not pinned */
    uint8_t         *base;
    size_t           bytes;
    uint64_t         gline0;            /* global cache-line index of base */
    uint64_t         rng;
    int              prep_error;        /* errno from mlock(), 0 if fine */
    bool             locked;
    _Alignas(64) uint64_t buf[BATCH_LINES * LINE_WORDS];
    _Alignas(64) uint64_t xbuf[BATCH_LINES * LINE_WORDS];
} worker_t;

uint64_t test_step_bytes(const tm5_test_t *t, size_t region, int passes, unsigned refresh_idle_ms);
void     test_run(worker_t *w, const step_t *s);

/* Provided by the runner. */
extern _Atomic int      g_stop;
extern bool             g_inject;
extern unsigned         g_inject_rate;  /* inject into 1 of every N written blocks */
extern _Atomic uint64_t g_injected;
void report_mismatch(worker_t *w, const step_t *s, uint64_t *addr, uint64_t expected, uint64_t actual);

#endif
