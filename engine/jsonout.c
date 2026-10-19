/* TM5 for Linux - line-delimited JSON output used by the GUI.
 * SPDX-License-Identifier: MIT
 */
#include "jsonout.h"
#include "util.h"

#include <math.h>
#include <string.h>
#include <unistd.h>

void jo_begin(FILE *f, const char *type)
{
    fputs("{\"type\":", f);
    json_str(f, type);
}

void jo_end(FILE *f)
{
    fputs("}\n", f);
    fflush(f);
}

static void key(FILE *f, const char *k)
{
    fputc(',', f);
    json_str(f, k);
    fputc(':', f);
}

void jo_int(FILE *f, const char *k, long long v)
{
    key(f, k);
    fprintf(f, "%lld", v);
}

void jo_u64(FILE *f, const char *k, uint64_t v)
{
    key(f, k);
    fprintf(f, "%llu", (unsigned long long)v);
}

void jo_dbl(FILE *f, const char *k, double v)
{
    key(f, k);
    if (isfinite(v))
        fprintf(f, "%.3f", v);
    else
        fputs("null", f);
}

void jo_str(FILE *f, const char *k, const char *v)
{
    key(f, k);
    json_str(f, v);
}

void jo_bool(FILE *f, const char *k, bool v)
{
    key(f, k);
    fputs(v ? "true" : "false", f);
}

void jo_hex(FILE *f, const char *k, uint64_t v)
{
    key(f, k);
    fprintf(f, "\"0x%016llx\"", (unsigned long long)v);
}

void jo_null(FILE *f, const char *k)
{
    key(f, k);
    fputs("null", f);
}

void jo_config(FILE *f, const tm5_config_t *cfg, int time_scale, unsigned refresh_idle_ms, int cycles)
{
    bool used[MAX_TESTS] = { false };
    for (int i = 0; i < cfg->nseq; i++)
        used[cfg->seq[i]] = true;

    jo_begin(f, "config");
    jo_str(f, "name", cfg->name);
    jo_str(f, "author", cfg->author);
    jo_str(f, "source", cfg->source);
    jo_int(f, "cores", cfg->cores);
    jo_int(f, "cycles", cycles >= 0 ? cycles : cfg->cycles);
    jo_int(f, "config_cycles", cfg->cycles);
    jo_int(f, "time_pct", cfg->time_pct);
    jo_int(f, "time_scale", time_scale);
    jo_int(f, "ntests", cfg->ntests);
    jo_u64(f, "reserved_mb", cfg->reserved_mb);

    fputs(",\"sequence\":[", f);
    for (int i = 0; i < cfg->nseq; i++)
        fprintf(f, "%s%d", i ? "," : "", cfg->seq[i]);
    fputs("],\"warnings\":[", f);
    for (int i = 0; i < cfg->nwarn; i++) {
        if (i)
            fputc(',', f);
        json_str(f, cfg->warn[i]);
    }
    fputs("],\"tests\":[", f);
    bool first = true;
    for (int i = 0; i < cfg->ntests; i++) {
        const tm5_test_t *t = &cfg->tests[i];
        if (!t->present)
            continue;
        char summary[256], explain[1024];
        test_summary(t, summary, sizeof summary);
        test_explain(t, refresh_idle_ms, explain, sizeof explain);
        fputs(first ? "{" : ",{", f);
        first = false;
        fprintf(f, "\"index\":%d", i);
        jo_str(f, "function", func_name(t->func));
        jo_str(f, "function_cfg", t->func_name);
        jo_bool(f, "enabled", t->enabled);
        jo_bool(f, "in_sequence", used[i]);
        jo_int(f, "time_pct", t->time_pct);
        jo_int(f, "passes", test_passes(cfg, t, time_scale));
        jo_int(f, "pattern_mode", t->pattern_mode);
        jo_hex(f, "param0", t->param0);
        jo_hex(f, "param1", t->param1);
        jo_u64(f, "parameter", t->parameter);
        jo_u64(f, "block_mb", t->block_mb);
        jo_str(f, "summary", summary);
        jo_str(f, "explain", explain);
        fputc('}', f);
    }
    fputc(']', f);
    jo_end(f);
}

void jo_sysinfo(FILE *f, const sysinfo_t *si, const tm5_config_t *cfg)
{
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = '\0';

    jo_begin(f, "sysinfo");
    jo_str(f, "version", TM5L_VERSION);
    jo_str(f, "engine", exe);
    jo_str(f, "cpu", si->cpu);
    jo_str(f, "arch", si->arch);
    jo_str(f, "kernel", si->kernel);
    jo_int(f, "cpus", si->cpus);
    jo_u64(f, "mem_total", si->mem_total);
    jo_u64(f, "mem_available", si->mem_available);
    jo_u64(f, "swap_total", si->swap_total);
    jo_u64(f, "swap_free", si->swap_free);
    jo_str(f, "thp", si->thp);
    if (si->memlock_limit == UINT64_MAX)
        jo_null(f, "memlock_limit");
    else
        jo_u64(f, "memlock_limit", si->memlock_limit);
    jo_bool(f, "cap_ipc_lock", si->cap_ipc_lock);
    jo_bool(f, "root", si->is_root);
    jo_bool(f, "can_lock", si->can_lock);
    jo_str(f, "lock_reason", si->lock_reason);
    jo_u64(f, "reserve", sysinfo_reserve(si, cfg ? cfg->reserved_mb : 128));
    jo_u64(f, "default_bytes", sysinfo_default_bytes(si, cfg ? cfg->reserved_mb : 128));
    double dimms[MAX_DIMM_SENSORS];
    int nd = sensors_dimms(&si->sensors, dimms, MAX_DIMM_SENSORS);
    jo_temps(f, &si->sensors, sensors_cpu(&si->sensors), dimms, nd);
    jo_end(f);
}

/* ,"cpu_temp":61.2,"cpu_sensor":"k10temp Tctl","dimm_temps":[45.0,46.5],"dimm_sensor":"spd5118" */
void jo_temps(FILE *f, const sensors_t *s, double cpu, const double *dimms, int ndimm)
{
    if (isnan(cpu))
        jo_null(f, "cpu_temp");
    else
        jo_dbl(f, "cpu_temp", cpu);
    jo_str(f, "cpu_sensor", s->cpu_name);
    fputs(",\"dimm_temps\":[", f);
    for (int i = 0; i < ndimm; i++) {
        if (i)
            fputc(',', f);
        if (isnan(dimms[i]))
            fputs("null", f);
        else
            fprintf(f, "%.1f", dimms[i]);
    }
    fputc(']', f);
    jo_str(f, "dimm_sensor", s->dimm_driver);
}

void jo_presets(FILE *f, const preset_t *p, int n)
{
    jo_begin(f, "presets");
    char udir[1024];
    config_user_dir(udir, sizeof udir);
    jo_str(f, "user_dir", udir);
    fputs(",\"presets\":[", f);
    for (int i = 0; i < n; i++) {
        fputs(i ? ",{" : "{", f);
        fputs("\"id\":", f);
        json_str(f, p[i].id);
        jo_str(f, "alias", p[i].alias);
        jo_str(f, "name", p[i].name);
        jo_str(f, "author", p[i].author);
        jo_str(f, "path", p[i].path);
        jo_int(f, "tests", p[i].ntests);
        jo_int(f, "steps", p[i].nseq);
        jo_int(f, "cycles", p[i].cycles);
        jo_bool(f, "user", p[i].user);
        fputc('}', f);
    }
    fputc(']', f);
    jo_end(f);
}
