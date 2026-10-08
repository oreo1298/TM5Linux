/* TM5 for Linux - shared types and constants.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_TM5_H
#define TM5L_TM5_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TM5L_VERSION
#define TM5L_VERSION "1.0.0"
#endif

#define LINE_BYTES   64u   /* one cache line = TM5 "Operation Block, byts=64" */
#define LINE_WORDS   8u
#define BATCH_LINES  64u   /* lines generated per pattern batch (4 KiB) */
#define MAX_TESTS    64
#define MAX_SEQ      512
#define MAX_WARN     16

/* Full memory passes done by a test whose effective "Time (%)" is 100.
 * Effective time = config Time (%) x test Time (%) x user time scale. */
#define PASSES_PER_UNIT 4

/* How long RefreshStable leaves memory untouched in each pass. */
#define REFRESH_IDLE_MS 4000u

typedef enum {
    FN_SIMPLE = 0,
    FN_REFRESH,
    FN_MIRROR,
    FN_MIRROR128,
} tm5_func_t;

typedef struct {
    bool       present;
    bool       enabled;
    tm5_func_t func;
    char       func_name[40];   /* as written in the config file */
    int        time_pct;
    int        pattern_mode;
    uint64_t   param0;
    uint64_t   param1;
    uint64_t   parameter;
    uint64_t   block_mb;
} tm5_test_t;

typedef struct {
    char       name[128];
    char       author[128];
    char       source[1024];    /* file path, or "built-in" */
    int        cores;           /* 0 = all logical CPUs */
    int        ntests;
    int        time_pct;
    int        cycles;          /* 0 = run until stopped */
    int        nseq;
    int        seq[MAX_SEQ];
    uint64_t   reserved_mb;
    tm5_test_t tests[MAX_TESTS];
    int        nwarn;
    char       warn[MAX_WARN][200];
} tm5_config_t;

typedef struct {
    const char *config;         /* preset name or path */
    uint64_t    mem_bytes;      /* 0 = automatic */
    double      mem_pct;        /* > 0: percentage of available memory */
    int         threads;        /* 0 = from config / all CPUs */
    int         cycles;         /* < 0 = from config, 0 = forever */
    int         time_scale;     /* percent, 100 = as the config says */
    bool        stop_on_error;
    bool        no_lock;
    bool        no_huge;
    bool        json;
    bool        quiet;          /* only print the final result */
    bool        silent;         /* print nothing at all (self-test) */
    bool        force;
    bool        inject;         /* self-test: simulate bit flips */
    unsigned    inject_rate;    /* ... in 1 of every N written blocks */
    const char *log_path;
    bool        no_log;
    uint64_t    seed;
    bool        seed_set;
    unsigned    refresh_idle_ms;
} options_t;

#endif
