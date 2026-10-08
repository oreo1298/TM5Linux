/* TM5 for Linux - small helpers.
 * SPDX-License-Identifier: MIT
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void fmt_bytes(uint64_t bytes, char *buf, size_t len)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, len, "%.*f %s", v < 10 ? 2 : v < 100 ? 1 : 0, v, units[u]);
}

void fmt_duration(double seconds, char *buf, size_t len)
{
    if (seconds < 0)
        seconds = 0;
    unsigned long s = (unsigned long)(seconds + 0.5);
    snprintf(buf, len, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

void fmt_clock(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%H:%M:%S", &tm);
}

void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
        }
    }
    fputc('"', f);
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[4096];
    str_copy(tmp, sizeof tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* splitmix64 finalizer: a fast, well-mixed 64-bit hash. */
uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

uint64_t rng_next(uint64_t *state)
{
    *state += 0x9e3779b97f4a7c15ULL;
    return mix64(*state);
}

char *str_trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

int strcasecmp_trim(const char *a, const char *b)
{
    char x[256], y[256];
    str_copy(x, sizeof x, a);
    str_copy(y, sizeof y, b);
    return strcasecmp(str_trim(x), str_trim(y));
}

void str_copy(char *dst, size_t len, const char *src)
{
    if (len == 0)
        return;
    size_t n = strlen(src);
    if (n >= len)
        n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}
