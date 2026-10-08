/* TM5 for Linux - TM5 .cfg parser, presets and plain-language test descriptions.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_CONFIG_H
#define TM5L_CONFIG_H

#include "tm5.h"

typedef struct {
    char id[128];      /* file name without ".cfg", e.g. "extreme@anta777" */
    char alias[64];    /* part before '@', e.g. "extreme" */
    char path[1024];
    char name[128];
    char author[128];
    int  ntests;
    int  nseq;
    int  cycles;
    bool user;         /* found in the user's config directory */
} preset_t;

int  config_parse(const char *text, const char *source, tm5_config_t *cfg, char *err, size_t errlen);
int  config_load_file(const char *path, tm5_config_t *cfg, char *err, size_t errlen);
int  config_resolve(const char *spec, tm5_config_t *cfg, char *err, size_t errlen);
int  presets_list(preset_t *out, int max);
void config_user_dir(char *buf, size_t len);

const char *func_name(tm5_func_t f);
void test_summary(const tm5_test_t *t, char *buf, size_t len);
void test_explain(const tm5_test_t *t, unsigned refresh_idle_ms, char *buf, size_t len);
int  test_passes(const tm5_config_t *cfg, const tm5_test_t *t, int time_scale);

#endif
