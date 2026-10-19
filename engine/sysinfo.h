/* TM5 for Linux - system information and memory allocation.
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_SYSINFO_H
#define TM5L_SYSINFO_H

#include "sensors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     cpu[160];
    char     arch[32];
    char     kernel[64];
    char     thp[32];          /* transparent hugepage mode: always / madvise / never */
    int      cpus;             /* CPUs this process may run on */
    uint64_t mem_total;
    uint64_t mem_available;
    uint64_t swap_total;
    uint64_t swap_free;
    uint64_t memlock_limit;    /* RLIMIT_MEMLOCK soft limit, UINT64_MAX = unlimited */
    bool     cap_ipc_lock;
    bool     is_root;
    bool     can_lock;         /* best guess whether mlock() of most RAM will work */
    char     lock_reason[160];
    sensors_t sensors;         /* CPU / DIMM temperature sensors */
} sysinfo_t;

void     sysinfo_read(sysinfo_t *si);
uint64_t sysinfo_reserve(const sysinfo_t *si, uint64_t cfg_reserved_mb);
uint64_t sysinfo_default_bytes(const sysinfo_t *si, uint64_t cfg_reserved_mb);
int      cpu_list(int *cpus, int max);

/* Memory */
void    *mem_map(size_t bytes, bool hugepages);
void     mem_unmap(void *p, size_t bytes);
uint64_t mem_virt_to_phys(const void *p);   /* 0 if unknown (needs root) */
uint64_t mem_anon_huge_bytes(void);

#endif
