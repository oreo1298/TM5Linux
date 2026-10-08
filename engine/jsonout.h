/* TM5 for Linux - line-delimited JSON output used by the GUI.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_JSONOUT_H
#define TM5L_JSONOUT_H

#include "config.h"
#include "sysinfo.h"

#include <stdio.h>

void jo_begin(FILE *f, const char *type);
void jo_end(FILE *f);
void jo_int(FILE *f, const char *k, long long v);
void jo_u64(FILE *f, const char *k, uint64_t v);
void jo_dbl(FILE *f, const char *k, double v);
void jo_str(FILE *f, const char *k, const char *v);
void jo_bool(FILE *f, const char *k, bool v);
void jo_hex(FILE *f, const char *k, uint64_t v);
void jo_null(FILE *f, const char *k);

void jo_config(FILE *f, const tm5_config_t *cfg, int time_scale, unsigned refresh_idle_ms, int cycles);
void jo_sysinfo(FILE *f, const sysinfo_t *si, const tm5_config_t *cfg);
void jo_presets(FILE *f, const preset_t *p, int n);

#endif
