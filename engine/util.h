/* TM5 for Linux - small helpers.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_UTIL_H
#define TM5L_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

double   now_sec(void);
void     fmt_bytes(uint64_t bytes, char *buf, size_t len);
void     fmt_duration(double seconds, char *buf, size_t len);
void     fmt_clock(char *buf, size_t len);              /* local time HH:MM:SS */
void     json_str(FILE *f, const char *s);
int      mkdir_p(const char *path, mode_t mode);
uint64_t mix64(uint64_t z);
uint64_t rng_next(uint64_t *state);
int      strcasecmp_trim(const char *a, const char *b);
char    *str_trim(char *s);
void     str_copy(char *dst, size_t len, const char *src);

#endif
