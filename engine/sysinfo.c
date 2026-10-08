/* TM5 for Linux - system information and memory allocation.
 * SPDX-License-Identifier: MIT
 */
#include "sysinfo.h"
#include "util.h"

#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CAP_IPC_LOCK_BIT 14

static uint64_t meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    char line[256];
    size_t klen = strlen(key);
    uint64_t v = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            v = strtoull(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static void read_cpu_model(char *buf, size_t len)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    buf[0] = '\0';
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Model", 5) == 0 ||
                strncmp(line, "cpu model", 9) == 0) {
                char *c = strchr(line, ':');
                if (c) {
                    str_copy(buf, len, str_trim(c + 1));
                    break;
                }
            }
        }
        fclose(f);
    }
    if (!buf[0])
        str_copy(buf, len, "Unknown CPU");
}

static bool read_cap_ipc_lock(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return false;
    char line[256];
    bool has = false;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "CapEff:", 7) == 0) {
            unsigned long long mask = strtoull(line + 7, NULL, 16);
            has = (mask >> CAP_IPC_LOCK_BIT) & 1;
            break;
        }
    }
    fclose(f);
    return has;
}

int cpu_list(int *cpus, int max)
{
    cpu_set_t set;
    int n = 0;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE && n < max; c++)
            if (CPU_ISSET(c, &set))
                cpus[n++] = c;
    }
    if (n == 0) {
        long nc = sysconf(_SC_NPROCESSORS_ONLN);
        for (int c = 0; c < nc && n < max; c++)
            cpus[n++] = c;
    }
    return n;
}

void sysinfo_read(sysinfo_t *si)
{
    memset(si, 0, sizeof *si);
    read_cpu_model(si->cpu, sizeof si->cpu);

    struct utsname u;
    if (uname(&u) == 0) {
        str_copy(si->arch, sizeof si->arch, u.machine);
        str_copy(si->kernel, sizeof si->kernel, u.release);
    }

    int cpus[1024];
    si->cpus = cpu_list(cpus, 1024);
    si->mem_total = meminfo_kb("MemTotal") * 1024;
    si->mem_available = meminfo_kb("MemAvailable") * 1024;
    if (!si->mem_available)
        si->mem_available = meminfo_kb("MemFree") * 1024;
    si->swap_total = meminfo_kb("SwapTotal") * 1024;
    si->swap_free = meminfo_kb("SwapFree") * 1024;

    str_copy(si->thp, sizeof si->thp, "unavailable");
    FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (f) {
        char line[128];
        if (fgets(line, sizeof line, f)) {
            char *a = strchr(line, '['), *b = a ? strchr(a, ']') : NULL;
            if (a && b) {
                *b = '\0';
                str_copy(si->thp, sizeof si->thp, a + 1);
            }
        }
        fclose(f);
    }

    struct rlimit rl;
    si->memlock_limit = 0;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
        si->memlock_limit = rl.rlim_cur == RLIM_INFINITY ? UINT64_MAX : (uint64_t)rl.rlim_cur;
    si->is_root = geteuid() == 0;
    si->cap_ipc_lock = read_cap_ipc_lock();

    if (si->is_root) {
        si->can_lock = true;
        str_copy(si->lock_reason, sizeof si->lock_reason, "running as root");
    } else if (si->cap_ipc_lock) {
        si->can_lock = true;
        str_copy(si->lock_reason, sizeof si->lock_reason, "CAP_IPC_LOCK capability granted");
    } else if (si->memlock_limit >= si->mem_total / 2) {
        si->can_lock = true;
        str_copy(si->lock_reason, sizeof si->lock_reason, "memlock limit is high enough");
    } else {
        char lim[32];
        fmt_bytes(si->memlock_limit, lim, sizeof lim);
        si->can_lock = false;
        snprintf(si->lock_reason, sizeof si->lock_reason,
                 "memlock limit is only %s and the CAP_IPC_LOCK capability is missing", lim);
    }
}

/* Memory left for the OS, desktop and this program. TM5 leaves 128 MB for Windows, but a Linux
 * desktop needs more headroom so it does not stall or trigger the OOM killer. */
uint64_t sysinfo_reserve(const sysinfo_t *si, uint64_t cfg_reserved_mb)
{
    uint64_t reserve = (384ull << 20) + si->mem_total / 50;
    if ((cfg_reserved_mb << 20) > reserve)
        reserve = cfg_reserved_mb << 20;
    return reserve;
}

uint64_t sysinfo_default_bytes(const sysinfo_t *si, uint64_t cfg_reserved_mb)
{
    uint64_t reserve = sysinfo_reserve(si, cfg_reserved_mb);
    if (si->mem_available <= reserve + (64ull << 20))
        return 64ull << 20;
    return (si->mem_available - reserve) & ~((1ull << 20) - 1);
}

void *mem_map(size_t bytes, bool hugepages)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
#ifdef MADV_HUGEPAGE
    if (hugepages)
        madvise(p, bytes, MADV_HUGEPAGE);
#else
    (void)hugepages;
#endif
#ifdef MADV_DONTDUMP
    madvise(p, bytes, MADV_DONTDUMP);
#endif
    return p;
}

void mem_unmap(void *p, size_t bytes)
{
    if (p)
        munmap(p, bytes);
}

uint64_t mem_virt_to_phys(const void *p)
{
    static int fd = -2;
    if (fd == -2)
        fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    long page = sysconf(_SC_PAGESIZE);
    uint64_t vaddr = (uint64_t)(uintptr_t)p;
    uint64_t entry = 0;
    if (pread(fd, &entry, sizeof entry, (off_t)(vaddr / (uint64_t)page * sizeof entry)) != sizeof entry)
        return 0;
    if (!(entry >> 63))
        return 0;                                   /* page not present */
    uint64_t pfn = entry & ((1ull << 55) - 1);
    if (!pfn)
        return 0;                                   /* hidden without CAP_SYS_ADMIN */
    return pfn * (uint64_t)page + vaddr % (uint64_t)page;
}

uint64_t mem_anon_huge_bytes(void)
{
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f)
        return 0;
    char line[256];
    uint64_t v = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "AnonHugePages:", 14) == 0) {
            v = strtoull(line + 14, NULL, 10) * 1024;
            break;
        }
    }
    fclose(f);
    return v;
}
