/* TM5 for Linux - CPU and memory (DIMM) temperature sensors via the kernel's hwmon interface.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_SENSORS_H
#define TM5L_SENSORS_H

#include <stdbool.h>

#define MAX_CPU_SENSORS  8
#define MAX_DIMM_SENSORS 16

typedef struct {
    int  ncpu;                              /* CPU readings (one per socket); the highest is used */
    char cpu_paths[MAX_CPU_SENSORS][320];
    char cpu_name[96];                      /* e.g. "k10temp Tctl", "coretemp Package id 0" */
    int  ndimm;                             /* one reading per memory module */
    char dimm_paths[MAX_DIMM_SENSORS][320];
    char dimm_driver[32];                   /* "spd5118" (DDR5) or "jc42" (DDR4) */
} sensors_t;

void   sensors_init(sensors_t *s);
double sensors_cpu(const sensors_t *s);                          /* degrees C, NAN if unknown */
int    sensors_dimms(const sensors_t *s, double *out, int max);  /* NAN entries if unreadable */
double sensors_dimm_max(const sensors_t *s);                     /* hottest module, NAN if none */

#endif
