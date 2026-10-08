/* TM5 for Linux - runs a config on all threads and reports progress.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_RUNNER_H
#define TM5L_RUNNER_H

#include "sysinfo.h"
#include "tm5.h"

typedef enum { RESULT_PASS = 0, RESULT_FAIL = 1, RESULT_STOPPED = 2, RESULT_ERROR = 3 } result_t;

typedef struct {
    result_t result;
    uint64_t errors;
    uint64_t injected;
    double   elapsed;
    int      cycles_done;
} run_result_t;

int  runner_run(const tm5_config_t *cfg, const options_t *opt, const sysinfo_t *si, run_result_t *res);
void runner_request_stop(void);

#endif
