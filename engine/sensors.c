/* TM5 for Linux - CPU and memory (DIMM) temperature sensors via the kernel's hwmon interface.
 * SPDX-License-Identifier: MIT
 *
 * CPU:  k10temp / zenpower (AMD), coretemp (Intel), cpu_thermal (ARM boards); otherwise the
 *       x86_pkg_temp thermal zone or, as a last resort, the ACPI thermal zone.
 * RAM:  spd5118 (DDR5 modules, Linux 6.11+) or jc42 (DDR4 modules with a sensor), one per module.
 *
 * Setting TM5LINUX_SYSFS points everything at another directory instead of /sys (used by tests).
 */
#include "sensors.h"
#include "util.h"

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *sysfs_root(void)
{
    const char *r = getenv("TM5LINUX_SYSFS");
    return r && *r ? r : "/sys";
}

static bool read_line(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(buf, (int)len, f) != NULL;
    fclose(f);
    if (ok) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
    }
    return ok;
}

/* Join two path parts; false if the result does not fit. */
static bool pjoin(char *out, size_t len, const char *a, const char *b)
{
    int n = snprintf(out, len, "%s/%s", a, b);
    return n > 0 && (size_t)n < len;
}

static double read_temp(const char *path)
{
    char buf[64];
    if (!read_line(path, buf, sizeof buf))
        return NAN;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf)
        return NAN;
    double c = (double)v / 1000.0;                 /* millidegrees */
    return (c > -60 && c < 200) ? c : NAN;          /* ignore obviously broken readings */
}

/* hwmon attributes live either in the hwmon directory itself or (old drivers) in device/. */
static bool attr_path(const char *dir, const char *attr, char *out, size_t len)
{
    char p[PATH_MAX], dev[PATH_MAX];
    if (pjoin(p, sizeof p, dir, attr) && access(p, R_OK) == 0) {
        str_copy(out, len, p);
        return true;
    }
    if (pjoin(dev, sizeof dev, dir, "device") && pjoin(p, sizeof p, dev, attr) && access(p, R_OK) == 0) {
        str_copy(out, len, p);
        return true;
    }
    return false;
}

static int cmp_hwmon(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    return atoi(x + 5) - atoi(y + 5);               /* "hwmon12" */
}

typedef struct {
    char path[320];
    long key;                                       /* i2c bus/address, for a stable module order */
} dimm_t;

static int cmp_dimm(const void *a, const void *b)
{
    const dimm_t *x = a, *y = b;
    return (x->key > y->key) - (x->key < y->key);
}

/* i2c devices are called "<bus>-<address>", e.g. "1-0051" for the second module on bus 1. */
static long dimm_key(const char *dir, long fallback)
{
    char link[PATH_MAX], real[PATH_MAX];
    if (!pjoin(link, sizeof link, dir, "device") || !realpath(link, real))
        return fallback;
    const char *base = strrchr(real, '/');
    base = base ? base + 1 : real;
    int bus = 0;
    unsigned addr = 0;
    if (sscanf(base, "%d-%x", &bus, &addr) == 2)
        return (long)bus * 0x10000 + (long)addr;
    return fallback;
}

void sensors_init(sensors_t *s)
{
    memset(s, 0, sizeof *s);
    char base[PATH_MAX];
    snprintf(base, sizeof base, "%s/class/hwmon", sysfs_root());

    DIR *d = opendir(base);
    char *names[256];
    int n = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n < 256)
            if (strncmp(de->d_name, "hwmon", 5) == 0)
                names[n++] = strdup(de->d_name);
        closedir(d);
    }
    qsort(names, (size_t)n, sizeof names[0], cmp_hwmon);

    int rank = 0;                                   /* quality of the CPU sensor found so far */
    dimm_t dimms[MAX_DIMM_SENSORS];
    int ndimm = 0;

    for (int i = 0; i < n; i++) {
        char dir[PATH_MAX], namefile[PATH_MAX], name[64] = "";
        if (!pjoin(dir, sizeof dir, base, names[i]) || !attr_path(dir, "name", namefile, sizeof namefile) ||
            !read_line(namefile, name, sizeof name))
            continue;

        if (strcmp(name, "spd5118") == 0 || strcmp(name, "jc42") == 0) {
            char p[320];
            if (ndimm < MAX_DIMM_SENSORS && attr_path(dir, "temp1_input", p, sizeof p)) {
                str_copy(dimms[ndimm].path, sizeof dimms[0].path, p);
                dimms[ndimm].key = dimm_key(dir, 0x7fffffffL - (MAX_DIMM_SENSORS - ndimm));
                ndimm++;
                str_copy(s->dimm_driver, sizeof s->dimm_driver, name);
            }
            continue;
        }

        int r = 0;
        if (strcmp(name, "k10temp") == 0 || strcmp(name, "zenpower") == 0 || strcmp(name, "coretemp") == 0)
            r = 4;
        else if (strcmp(name, "cpu_thermal") == 0 || strcmp(name, "cpu-thermal") == 0 ||
                 strcmp(name, "soc_thermal") == 0 || strcmp(name, "cpu0_thermal") == 0)
            r = 3;
        else if (strcmp(name, "acpitz") == 0)
            r = 1;
        if (r == 0 || r < rank)
            continue;

        /* Pick the best channel: Tdie/Tctl on AMD, the package sensor on Intel, else temp1. */
        static const char *prefer[] = { "Tdie", "Tctl", "Package id", "CPU", NULL };
        char best[320] = "", best_label[64] = "";
        int best_pref = 99;
        for (int ch = 1; ch <= 32; ch++) {
            char attr[32], in[320], lab[320], label[64] = "";
            snprintf(attr, sizeof attr, "temp%d_input", ch);
            if (!attr_path(dir, attr, in, sizeof in))
                continue;
            snprintf(attr, sizeof attr, "temp%d_label", ch);
            if (attr_path(dir, attr, lab, sizeof lab))
                read_line(lab, label, sizeof label);
            int pref = 50 + ch;
            for (int k = 0; prefer[k]; k++)
                if (strncasecmp(label, prefer[k], strlen(prefer[k])) == 0) {
                    pref = k;
                    break;
                }
            if (pref < best_pref) {
                best_pref = pref;
                str_copy(best, sizeof best, in);
                str_copy(best_label, sizeof best_label, label);
            }
        }
        if (!best[0])
            continue;
        if (r > rank) {                             /* a better kind of sensor: start over */
            rank = r;
            s->ncpu = 0;
            snprintf(s->cpu_name, sizeof s->cpu_name, "%s%s%s", name, best_label[0] ? " " : "", best_label);
        }
        if (s->ncpu < MAX_CPU_SENSORS)              /* multi-socket systems: one per CPU */
            str_copy(s->cpu_paths[s->ncpu++], sizeof s->cpu_paths[0], best);
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    if (s->ncpu > 1) {                              /* e.g. "coretemp Package id 0" -> "coretemp" */
        char drv[64];
        str_copy(drv, sizeof drv, s->cpu_name);
        char *sp = strchr(drv, ' ');
        if (sp)
            *sp = '\0';
        snprintf(s->cpu_name, sizeof s->cpu_name, "%s, hottest of %d CPUs", drv, s->ncpu);
    }

    /* Intel without the coretemp module: the package thermal zone still has the temperature. */
    if (rank < 4) {
        char tbase[PATH_MAX];
        snprintf(tbase, sizeof tbase, "%s/class/thermal", sysfs_root());
        DIR *td = opendir(tbase);
        if (td) {
            struct dirent *de;
            while ((de = readdir(td))) {
                if (strncmp(de->d_name, "thermal_zone", 12) != 0)
                    continue;
                char zone[PATH_MAX], p[PATH_MAX], type[64] = "";
                if (!pjoin(zone, sizeof zone, tbase, de->d_name) || !pjoin(p, sizeof p, zone, "type") ||
                    !read_line(p, type, sizeof type) || strcmp(type, "x86_pkg_temp") != 0)
                    continue;
                if (!pjoin(p, sizeof p, zone, "temp") || access(p, R_OK) != 0)
                    continue;
                if (rank < 2) {
                    rank = 2;
                    s->ncpu = 0;
                    str_copy(s->cpu_name, sizeof s->cpu_name, "x86_pkg_temp");
                }
                if (s->ncpu < MAX_CPU_SENSORS)
                    str_copy(s->cpu_paths[s->ncpu++], sizeof s->cpu_paths[0], p);
            }
            closedir(td);
        }
    }

    qsort(dimms, (size_t)ndimm, sizeof dimms[0], cmp_dimm);
    for (int i = 0; i < ndimm; i++)
        str_copy(s->dimm_paths[i], sizeof s->dimm_paths[0], dimms[i].path);
    s->ndimm = ndimm;
}

double sensors_cpu(const sensors_t *s)
{
    double best = NAN;
    for (int i = 0; i < s->ncpu; i++) {
        double t = read_temp(s->cpu_paths[i]);
        if (!isnan(t) && (isnan(best) || t > best))
            best = t;
    }
    return best;
}

int sensors_dimms(const sensors_t *s, double *out, int max)
{
    int n = s->ndimm < max ? s->ndimm : max;
    for (int i = 0; i < n; i++)
        out[i] = read_temp(s->dimm_paths[i]);
    return n;
}

double sensors_dimm_max(const sensors_t *s)
{
    double t[MAX_DIMM_SENSORS], best = NAN;
    int n = sensors_dimms(s, t, MAX_DIMM_SENSORS);
    for (int i = 0; i < n; i++)
        if (!isnan(t[i]) && (isnan(best) || t[i] > best))
            best = t[i];
    return best;
}
