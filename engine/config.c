/* TM5 for Linux - TM5 .cfg parser, presets and plain-language test descriptions.
 * SPDX-License-Identifier: MIT
 */
#include "config.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef TM5L_DATADIR
#define TM5L_DATADIR "/usr/local/share/tm5linux"
#endif

/* Built-in copy of "Extreme1 @anta777" so the engine works even without its data files. */
static const char builtin_extreme[] =
    "[Main Section]\n"
    "Config Name=Extreme1\nConfig Author=anta777\nCores=0\nTests=16\nTime (%)=500\nCycles=3\n"
    "Test Sequence=4,6,1,12,2,4,12,2,5,7,1,13,2,5,13,2,8,9,1,14,2,8,14,2,10,11,1,15,2,10,3\n"
    "[Global Memory Setup]\nReserved Memory for Windows (Mb)=128\n"
    "[Test0]\nTime (%)=100\nFunction=RefreshStable\nPattern Mode=0\nParameter=0\nTest Block Size (Mb)=0\n"
    "[Test1]\nTime (%)=20\nFunction=MirrorMove\nPattern Mode=0\nParameter=4\nTest Block Size (Mb)=0\n"
    "[Test2]\nTime (%)=100\nFunction=MirrorMove128\nPattern Mode=0\nParameter=2\nTest Block Size (Mb)=0\n"
    "[Test3]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=1\nPattern Param0=0x1E5F\nPattern Param1=0x45357354\nParameter=256\nTest Block Size (Mb)=4\n"
    "[Test4]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=0\nParameter=0\nTest Block Size (Mb)=4\n"
    "[Test5]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=0\nParameter=0\nTest Block Size (Mb)=0\n"
    "[Test6]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=0\nParameter=2\nTest Block Size (Mb)=4\n"
    "[Test7]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=0\nParameter=2\nTest Block Size (Mb)=0\n"
    "[Test8]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=2\nPattern Param0=0x1E5F\nPattern Param1=0x45357354\nParameter=0\nTest Block Size (Mb)=4\n"
    "[Test9]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=2\nPattern Param0=0x2305B\nPattern Param1=0x97893FB2\nParameter=2\nTest Block Size (Mb)=4\n"
    "[Test10]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=2\nPattern Param0=0x98FB\nPattern Param1=0x552FE552\nParameter=0\nTest Block Size (Mb)=0\n"
    "[Test11]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=2\nPattern Param0=0xC51C\nPattern Param1=0xC5052FE6\nParameter=2\nTest Block Size (Mb)=0\n"
    "[Test12]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=0\nParameter=256\nTest Block Size (Mb)=4\n"
    "[Test13]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=0\nParameter=256\nTest Block Size (Mb)=0\n"
    "[Test14]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=2\nPattern Param0=0xB79D9\nPattern Param1=0x253B69D4\nParameter=256\nTest Block Size (Mb)=4\n"
    "[Test15]\nTime (%)=100\nFunction=SimpleTest\nPattern Mode=2\nPattern Param0=0x2305A\nPattern Param1=0x1789AB54\nParameter=256\nTest Block Size (Mb)=0\n";

static void add_warn(tm5_config_t *cfg, const char *fmt, const char *a, long b)
{
    if (cfg->nwarn >= MAX_WARN)
        return;
    snprintf(cfg->warn[cfg->nwarn++], sizeof cfg->warn[0], fmt, a, b);
}

static bool key_is(const char *key, const char *prefix)
{
    return strncasecmp(key, prefix, strlen(prefix)) == 0;
}

static uint64_t to_u64(const char *v)
{
    return strtoull(v, NULL, 0);
}

static int to_int(const char *v)
{
    long x = strtol(v, NULL, 0);
    if (x < 0)
        x = 0;
    if (x > INT_MAX)
        x = INT_MAX;
    return (int)x;
}

static bool parse_func(const char *v, tm5_func_t *out)
{
    static const struct { const char *name; tm5_func_t f; } map[] = {
        { "SimpleTest", FN_SIMPLE },     { "RefreshStable", FN_REFRESH },
        { "MirrorMove", FN_MIRROR },     { "MirrorMove128", FN_MIRROR128 },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcasecmp(v, map[i].name) == 0) {
            *out = map[i].f;
            return true;
        }
    }
    return false;
}

const char *func_name(tm5_func_t f)
{
    switch (f) {
    case FN_SIMPLE:    return "SimpleTest";
    case FN_REFRESH:   return "RefreshStable";
    case FN_MIRROR:    return "MirrorMove";
    case FN_MIRROR128: return "MirrorMove128";
    }
    return "?";
}

enum { SEC_NONE, SEC_MAIN, SEC_GLOBAL, SEC_TEST, SEC_OTHER };

int config_parse(const char *text, const char *source, tm5_config_t *cfg, char *err, size_t errlen)
{
    memset(cfg, 0, sizeof *cfg);
    str_copy(cfg->name, sizeof cfg->name, "Unnamed");
    str_copy(cfg->author, sizeof cfg->author, "unknown");
    str_copy(cfg->source, sizeof cfg->source, source);
    cfg->time_pct = 100;
    cfg->cycles = 3;
    cfg->reserved_mb = 128;
    cfg->ntests = -1;

    char *copy = strdup(text);
    if (!copy) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }

    int sec = SEC_NONE, cur = -1, max_index = -1;
    bool have_seq = false;
    char seqbuf[4096] = "";
    char *save = NULL;

    /* TM5 configs come with CRLF, LF or even bare CR line endings. */
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *l = str_trim(line);
        if (*l == '\0' || *l == ';' || *l == '#')
            continue;
        if (*l == '[') {
            char *end = strchr(l, ']');
            if (end)
                *end = '\0';
            char *name = str_trim(l + 1);
            cur = -1;
            if (strcasecmp(name, "Main Section") == 0) {
                sec = SEC_MAIN;
            } else if (strcasecmp(name, "Global Memory Setup") == 0) {
                sec = SEC_GLOBAL;
            } else if (strncasecmp(name, "Test", 4) == 0 && name[4] >= '0' && name[4] <= '9') {
                long idx = strtol(name + 4, NULL, 10);
                if (idx >= 0 && idx < MAX_TESTS) {
                    sec = SEC_TEST;
                    cur = (int)idx;
                    tm5_test_t *t = &cfg->tests[cur];
                    memset(t, 0, sizeof *t);
                    t->present = true;
                    t->enabled = true;
                    t->time_pct = 100;
                    t->func = FN_SIMPLE;
                    str_copy(t->func_name, sizeof t->func_name, "SimpleTest");
                    if (cur > max_index)
                        max_index = cur;
                } else {
                    sec = SEC_OTHER;
                    add_warn(cfg, "Ignoring section [%s]: only Test0..Test%ld are supported", name, MAX_TESTS - 1);
                }
            } else {
                sec = SEC_OTHER;
            }
            continue;
        }

        char *eq = strchr(l, '=');
        if (!eq)
            continue;               /* the free-text TM5 header lines */
        *eq = '\0';
        char *key = str_trim(l);
        char *val = eq + 1;
        char *comment = strchr(val, ';');
        if (comment)
            *comment = '\0';
        val = str_trim(val);

        if (sec == SEC_MAIN) {
            if (key_is(key, "Config Name"))
                str_copy(cfg->name, sizeof cfg->name, val);
            else if (key_is(key, "Config Author"))
                str_copy(cfg->author, sizeof cfg->author, val);
            else if (key_is(key, "Cores"))
                cfg->cores = to_int(val);
            else if (key_is(key, "Tests"))
                cfg->ntests = to_int(val);
            else if (key_is(key, "Time"))
                cfg->time_pct = to_int(val);
            else if (key_is(key, "Cycles"))
                cfg->cycles = to_int(val);
            else if (key_is(key, "Test Sequence")) {
                str_copy(seqbuf, sizeof seqbuf, val);
                have_seq = true;
            }
        } else if (sec == SEC_GLOBAL) {
            if (key_is(key, "Reserved Memory"))
                cfg->reserved_mb = to_u64(val);
        } else if (sec == SEC_TEST && cur >= 0) {
            tm5_test_t *t = &cfg->tests[cur];
            if (key_is(key, "Enable"))
                t->enabled = to_int(val) != 0;
            else if (key_is(key, "Time"))
                t->time_pct = to_int(val);
            else if (key_is(key, "Function")) {
                str_copy(t->func_name, sizeof t->func_name, val);
                if (!parse_func(val, &t->func)) {
                    t->func = FN_SIMPLE;
                    add_warn(cfg, "Test function \"%s\" (Test%ld) is not known; SimpleTest is used instead", val, cur);
                }
            } else if (key_is(key, "Pattern Mode"))
                t->pattern_mode = to_int(val);
            else if (key_is(key, "Pattern Param0"))
                t->param0 = to_u64(val);
            else if (key_is(key, "Pattern Param1"))
                t->param1 = to_u64(val);
            else if (key_is(key, "Parameter"))
                t->parameter = to_u64(val);
            else if (key_is(key, "Test Block Size"))
                t->block_mb = to_u64(val);
        }
    }
    free(copy);

    if (cfg->ntests < 0 || cfg->ntests > max_index + 1)
        cfg->ntests = max_index + 1;
    if (cfg->ntests <= 0) {
        snprintf(err, errlen, "%s: no [TestN] sections found - is this a TM5 config file?", source);
        return -1;
    }

    if (have_seq) {
        char *s2 = NULL;
        for (char *tok = strtok_r(seqbuf, ", \t", &s2); tok; tok = strtok_r(NULL, ", \t", &s2)) {
            char *end = NULL;
            long idx = strtol(tok, &end, 10);
            if (end == tok)
                continue;
            if (idx < 0 || idx >= cfg->ntests || !cfg->tests[idx].present) {
                add_warn(cfg, "Test Sequence refers to missing test %s%ld - skipped", "#", idx);
                continue;
            }
            if (!cfg->tests[idx].enabled)
                continue;
            if (cfg->nseq < MAX_SEQ)
                cfg->seq[cfg->nseq++] = (int)idx;
        }
    }
    if (cfg->nseq == 0) {
        for (int i = 0; i < cfg->ntests && cfg->nseq < MAX_SEQ; i++)
            if (cfg->tests[i].present && cfg->tests[i].enabled)
                cfg->seq[cfg->nseq++] = i;
    }
    if (cfg->nseq == 0) {
        snprintf(err, errlen, "%s: all tests are disabled", source);
        return -1;
    }
    if (cfg->time_pct <= 0)
        cfg->time_pct = 100;
    return 0;
}

int config_load_file(const char *path, tm5_config_t *cfg, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    char *buf = malloc(1 << 20);
    if (!buf) {
        fclose(f);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char real[PATH_MAX];
    int rc = config_parse(buf, realpath(path, real) ? real : path, cfg, err, errlen);
    free(buf);
    return rc;
}

/* ---------------------------------------------------------------------------------------------
 * Presets: *.cfg files in the user's config dir and in the installed data dir.
 * ------------------------------------------------------------------------------------------- */

void config_user_dir(char *buf, size_t len)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg == '/')
        snprintf(buf, len, "%s/tm5linux/configs", xdg);
    else if (home && *home)
        snprintf(buf, len, "%s/.config/tm5linux/configs", home);
    else
        buf[0] = '\0';
}

static int preset_dirs(char dirs[][1024], bool *user, int max)
{
    int n = 0;
    char exe[PATH_MAX];
    char cand[4][1024];
    int nc = 0;

    config_user_dir(cand[nc++], sizeof cand[0]);
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            if (snprintf(cand[nc], sizeof cand[0], "%s/../share/tm5linux/configs", exe) < (int)sizeof cand[0])
                nc++;
            if (snprintf(cand[nc], sizeof cand[0], "%s/../configs", exe) < (int)sizeof cand[0])
                nc++;
        }
    }
    snprintf(cand[nc++], sizeof cand[0], "%s/configs", TM5L_DATADIR);

    for (int i = 0; i < nc && n < max; i++) {
        char real[PATH_MAX];
        if (!cand[i][0] || !realpath(cand[i], real))
            continue;
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (strcmp(dirs[j], real) == 0)
                dup = true;
        if (dup)
            continue;
        str_copy(dirs[n], 1024, real);
        user[n] = (i == 0);
        n++;
    }
    return n;
}

static int cmp_preset(const void *a, const void *b)
{
    const preset_t *x = a, *y = b;
    /* Extreme first (it is the default), then alphabetical. */
    bool xe = strcasecmp(x->alias, "extreme") == 0, ye = strcasecmp(y->alias, "extreme") == 0;
    if (xe != ye)
        return xe ? -1 : 1;
    return strcasecmp(x->id, y->id);
}

int presets_list(preset_t *out, int max)
{
    char dirs[4][1024];
    bool user[4];
    int nd = preset_dirs(dirs, user, 4);
    int n = 0;

    for (int d = 0; d < nd; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir)
            continue;
        struct dirent *de;
        while ((de = readdir(dir)) && n < max) {
            size_t len = strlen(de->d_name);
            if (len < 5 || strcasecmp(de->d_name + len - 4, ".cfg") != 0)
                continue;
            preset_t p;
            memset(&p, 0, sizeof p);
            if (snprintf(p.path, sizeof p.path, "%s/%s", dirs[d], de->d_name) >= (int)sizeof p.path)
                continue;
            str_copy(p.id, sizeof p.id, de->d_name);
            p.id[len - 4] = '\0';
            /* A file in the user dir with the same name overrides the bundled one. */
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (strcasecmp(out[i].id, p.id) == 0)
                    dup = true;
            if (dup)
                continue;
            str_copy(p.alias, sizeof p.alias, p.id);
            char *at = strchr(p.alias, '@');
            if (at)
                *at = '\0';
            tm5_config_t *cfg = malloc(sizeof *cfg);
            char err[256];
            if (!cfg)
                break;
            if (config_load_file(p.path, cfg, err, sizeof err) == 0) {
                str_copy(p.name, sizeof p.name, cfg->name);
                str_copy(p.author, sizeof p.author, cfg->author);
                p.ntests = cfg->ntests;
                p.nseq = cfg->nseq;
                p.cycles = cfg->cycles;
                p.user = user[d];
                out[n++] = p;
            }
            free(cfg);
        }
        closedir(dir);
    }

    bool have_extreme = false;
    for (int i = 0; i < n; i++)
        if (strcasecmp(out[i].alias, "extreme") == 0)
            have_extreme = true;
    if (!have_extreme && n < max) {
        preset_t p;
        memset(&p, 0, sizeof p);
        str_copy(p.id, sizeof p.id, "extreme@anta777");
        str_copy(p.alias, sizeof p.alias, "extreme");
        str_copy(p.path, sizeof p.path, "built-in");
        str_copy(p.name, sizeof p.name, "Extreme1");
        str_copy(p.author, sizeof p.author, "anta777");
        p.ntests = 16;
        p.nseq = 31;
        p.cycles = 3;
        out[n++] = p;
    }
    qsort(out, (size_t)n, sizeof out[0], cmp_preset);
    return n;
}

int config_resolve(const char *spec, tm5_config_t *cfg, char *err, size_t errlen)
{
    if (!spec || !*spec)
        spec = "extreme";
    if (strchr(spec, '/') || access(spec, R_OK) == 0)
        return config_load_file(spec, cfg, err, errlen);

    preset_t *list = calloc(128, sizeof *list);
    if (!list) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int n = presets_list(list, 128);
    int found = -1;
    for (int i = 0; i < n && found < 0; i++)
        if (strcasecmp(list[i].id, spec) == 0)
            found = i;
    for (int i = 0; i < n && found < 0; i++)
        if (strcasecmp(list[i].alias, spec) == 0 || strcasecmp(list[i].name, spec) == 0)
            found = i;
    int rc;
    if (found >= 0 && strcmp(list[found].path, "built-in") != 0) {
        rc = config_load_file(list[found].path, cfg, err, errlen);
    } else if (found >= 0 || strncasecmp(spec, "extreme", 7) == 0) {
        rc = config_parse(builtin_extreme, "built-in", cfg, err, errlen);
    } else {
        snprintf(err, errlen, "no preset or file called \"%s\" (try --list-presets)", spec);
        rc = -1;
    }
    free(list);
    return rc;
}

/* ---------------------------------------------------------------------------------------------
 * Plain-language descriptions, shared by the terminal UI and the GUI (via --describe --json).
 * ------------------------------------------------------------------------------------------- */

static void fmt_size_short(uint64_t bytes, char *buf, size_t len)
{
    if (bytes >= (1ull << 20) && bytes % (1ull << 20) == 0)
        snprintf(buf, len, "%llu MiB", (unsigned long long)(bytes >> 20));
    else if (bytes >= 1024 && bytes % 1024 == 0)
        snprintf(buf, len, "%llu KiB", (unsigned long long)(bytes >> 10));
    else
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
}

static const char *pattern_label(int mode)
{
    switch (mode) {
    case 0:  return "Classic patterns";
    case 1:  return "Random data";
    default: return "Seeded sequence";
    }
}

static const char *pattern_text(int mode)
{
    switch (mode) {
    case 0:  return "classic test patterns (all zeros/ones, checkerboards, walking bits, own address)";
    case 1:  return "random data";
    default: return "a seeded number sequence";
    }
}

void test_summary(const tm5_test_t *t, char *buf, size_t len)
{
    char a[32], b[32];
    uint64_t unit = t->parameter > 1 ? t->parameter : 1;
    fmt_size_short(t->block_mb << 20, a, sizeof a);
    fmt_size_short(unit * LINE_BYTES, b, sizeof b);

    switch (t->func) {
    case FN_SIMPLE:
        snprintf(buf, len, "%s · %s · %s%s", pattern_label(t->pattern_mode),
                 t->block_mb ? a : "whole range",
                 t->parameter > 1 ? b : "sequential", t->parameter > 1 ? " stride" : "");
        break;
    case FN_MIRROR:
    case FN_MIRROR128:
        snprintf(buf, len, "%s copy · %s chunks · %s", t->func == FN_MIRROR ? "64-bit" : "128-bit",
                 b, pattern_label(t->pattern_mode == 0 ? 1 : t->pattern_mode));
        break;
    case FN_REFRESH:
        snprintf(buf, len, "Hold & verify · %s", pattern_label(t->pattern_mode));
        break;
    }
}

void test_explain(const tm5_test_t *t, unsigned refresh_idle_ms, char *buf, size_t len)
{
    char blk[32], step[32];
    uint64_t unit = t->parameter > 1 ? t->parameter : 1;
    fmt_size_short(t->block_mb << 20, blk, sizeof blk);
    fmt_size_short(unit * LINE_BYTES, step, sizeof step);

    switch (t->func) {
    case FN_SIMPLE: {
        char access[260];
        if (t->parameter > 1)
            snprintf(access, sizeof access,
                     "Each access jumps %s ahead, which forces the RAM to keep opening new rows - this stresses "
                     "row timings such as tRCD, tRP, tRAS and tRC.", step);
        else
            snprintf(access, sizeof access,
                     "Memory is walked in order at full speed, which stresses bandwidth and the data bus.");
        if (t->block_mb)
            snprintf(buf, len, "Writes %s into one %s block at a time and checks every bit of each block right "
                     "after writing it. %s", pattern_text(t->pattern_mode), blk, access);
        else
            snprintf(buf, len, "Writes %s across all of the memory first and only then reads it back and checks "
                     "every bit, so the data has to survive for a while. %s", pattern_text(t->pattern_mode), access);
        break;
    }
    case FN_MIRROR:
    case FN_MIRROR128:
        snprintf(buf, len,
                 "Fills half of the memory with %s, copies it to the other half in mirrored order (%s pieces, "
                 "reversed and bit-inverted) using %s moves, verifies the copy, copies it back and verifies again. "
                 "The constant mix of reads and writes stresses read/write turnaround timings (tWTR, tRTW, tWR) "
                 "and the memory controller.",
                 t->pattern_mode == 0 ? "random data" : pattern_text(t->pattern_mode), step,
                 t->func == FN_MIRROR ? "64-bit" : "128-bit SSE");
        break;
    case FN_REFRESH:
        snprintf(buf, len,
                 "Fills memory with %s, leaves it completely untouched for %u seconds so the RAM has to keep the "
                 "data alive with refresh alone, then checks it. Finds refresh problems (tREFI, tRFC) and weak cells.",
                 pattern_text(t->pattern_mode), (refresh_idle_ms + 500) / 1000);
        break;
    }
}

int test_passes(const tm5_config_t *cfg, const tm5_test_t *t, int time_scale)
{
    double eff = (double)cfg->time_pct / 100.0 * (double)t->time_pct / 100.0 * (double)time_scale / 100.0;
    double passes = eff * PASSES_PER_UNIT;
    int p = (int)(passes + 0.5);
    return p < 1 ? 1 : p;
}
