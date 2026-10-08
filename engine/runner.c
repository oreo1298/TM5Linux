/* TM5 for Linux - runs a config on all threads and reports progress.
 * SPDX-License-Identifier: MIT
 *
 * Like TM5, every test of the sequence runs on all threads at the same time; each thread tests its
 * own slice of the memory and the next test starts when every thread has finished.
 */
#include "runner.h"
#include "arch.h"
#include "config.h"
#include "jsonout.h"
#include "tests.h"
#include "ui.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

_Atomic int      g_stop;
bool             g_inject;
unsigned         g_inject_rate = 1;
_Atomic uint64_t g_injected;

#define MAX_THREADS      1024
#define ERROR_DETAIL_CAP 1000       /* detailed error reports per run; the rest are only counted */
#define TTY_ERROR_EVENTS 50

static const char *lv_name[] = { "info", "ok", "warn", "error" };
static const char *lv_tag[] = { "INFO", "OK", "WARN", "ERROR" };

static struct {
    const tm5_config_t *cfg;
    const options_t    *opt;
    int                 nthreads;
    size_t              per;
    uint64_t            total;
    uint8_t            *map;
    size_t              map_len;
    worker_t           *w;
    pthread_t          *tid;
    pthread_barrier_t   bar;
    _Atomic int         done_count;
    _Atomic int         quit;
    _Atomic int         go;
    bool                want_lock;
    step_t              step;
    _Atomic uint64_t    errors;
    _Atomic uint64_t    step_errors;
    _Atomic uint64_t    test_errors[MAX_TESTS];
    pthread_mutex_t     out;
    FILE               *logf;
    bool                json;
    bool                tty;
    double              t0;
    ui_state_t          ui;
    char                marks[MAX_SEQ + 1];
    uint64_t            step_bytes[MAX_SEQ];    /* bytes processed by one step, all threads */
    double              rate[MAX_TESTS];        /* measured bytes/s per test number */
    double              test_bytes_done;        /* in completed steps */
    double              test_secs_done;
} R;

void runner_request_stop(void)
{
    atomic_store(&g_stop, 1);
}

/* ---- output helpers -------------------------------------------------------------------------- */

static void logline(const char *tag, const char *text)
{
    if (!R.logf)
        return;
    char clk[16];
    fmt_clock(clk, sizeof clk);
    fprintf(R.logf, "%s  %-5s %s\n", clk, tag, text);
    fflush(R.logf);
}

/* A plain message: a JSON "log" event for the GUI, or a line in the terminal UI. */
static void say(int lv, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int lv, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&R.out);
    if (R.json) {
        jo_begin(stdout, "log");
        jo_str(stdout, "level", lv_name[lv]);
        jo_str(stdout, "msg", text);
        jo_end(stdout);
    } else if (!R.opt->quiet) {
        ui_event(lv, text);
    }
    logline(lv_tag[lv], text);
    pthread_mutex_unlock(&R.out);
}

/* Human-only message (the GUI builds its own text from the structured JSON events). */
static void human(int lv, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void human(int lv, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (!R.json && !R.opt->quiet)
        ui_event(lv, text);
    logline(lv_tag[lv], text);
}

/* ---- error reporting (called from worker threads) -------------------------------------------- */

void report_mismatch(worker_t *w, const step_t *s, uint64_t *addr, uint64_t expected, uint64_t actual)
{
    uint64_t n = atomic_fetch_add(&R.errors, 1) + 1;
    atomic_fetch_add(&w->errors, 1);
    atomic_fetch_add(&R.step_errors, 1);
    atomic_fetch_add(&R.test_errors[s->index], 1);
    if (R.opt->stop_on_error)
        atomic_store(&g_stop, 1);

    if (n > ERROR_DETAIL_CAP) {
        if (n == ERROR_DETAIL_CAP + 1)
            say(LV_ERR, "More than %d errors - further errors are only counted, not listed.", ERROR_DETAIL_CAP);
        return;
    }

    /* Read the word again straight from DRAM: if it is now correct, the error happened while
     * reading (signal/timing problem); if it is still wrong, the stored data itself is bad. */
    flush_line(addr);
    uint64_t r1 = *(volatile uint64_t *)addr;
    flush_line(addr);
    uint64_t r2 = *(volatile uint64_t *)addr;
    bool persistent = r1 != expected || r2 != expected;

    uint64_t diff = expected ^ actual;
    int bits = __builtin_popcountll(diff);
    char bitlist[160] = "";
    size_t bl = 0;
    int shown = 0;
    for (int b = 0; b < 64 && shown < 8; b++) {
        if (!((diff >> b) & 1))
            continue;
        bl += (size_t)snprintf(bitlist + bl, sizeof bitlist - bl, "%s%d", shown ? ", " : "", b);
        shown++;
    }
    if (bits > shown)
        snprintf(bitlist + bl, sizeof bitlist - bl, ", ...");

    uint64_t vaddr = (uint64_t)(uintptr_t)addr;
    uint64_t offset = (uint64_t)((uint8_t *)addr - R.w[0].base);
    uint64_t paddr = mem_virt_to_phys(addr);
    double el = now_sec() - R.t0;

    pthread_mutex_lock(&R.out);
    if (R.json) {
        jo_begin(stdout, "error");
        jo_u64(stdout, "n", n);
        jo_int(stdout, "cycle", s->cycle);
        jo_int(stdout, "step", s->step);
        jo_int(stdout, "test", s->index);
        jo_str(stdout, "function", func_name(s->t->func));
        jo_int(stdout, "thread", w->id);
        jo_int(stdout, "cpu", w->cpu);
        jo_hex(stdout, "vaddr", vaddr);
        if (paddr)
            jo_hex(stdout, "paddr", paddr);
        else
            jo_null(stdout, "paddr");
        jo_hex(stdout, "offset", offset);
        jo_hex(stdout, "expected", expected);
        jo_hex(stdout, "actual", actual);
        jo_hex(stdout, "xor", diff);
        jo_int(stdout, "bits", bits);
        jo_str(stdout, "bit_list", bitlist);
        jo_str(stdout, "kind", persistent ? "persistent" : "transient");
        jo_dbl(stdout, "elapsed", el);
        jo_end(stdout);
    }
    char text[512], where[64] = "";
    if (paddr)
        snprintf(where, sizeof where, " (physical 0x%llx)", (unsigned long long)paddr);
    snprintf(text, sizeof text,
             "ERROR #%llu in test #%d (%s), thread %d: offset 0x%llx%s - expected 0x%016llx, read 0x%016llx - "
             "%d bit%s wrong (bit %s) - %s",
             (unsigned long long)n, s->index, func_name(s->t->func), w->id, (unsigned long long)offset, where,
             (unsigned long long)expected, (unsigned long long)actual, bits, bits == 1 ? "" : "s", bitlist,
             persistent ? "data in RAM is wrong" : "read error, re-read was correct");
    if (!R.json && !R.opt->quiet && (n <= TTY_ERROR_EVENTS || !R.tty))
        ui_event(LV_ERR, text);
    logline("ERROR", text);
    pthread_mutex_unlock(&R.out);
}

/* ---- worker threads -------------------------------------------------------------------------- */

static void prepare_region(worker_t *w)
{
    const size_t chunk = 64ull << 20;
    size_t locked = 0;
    if (R.want_lock) {
        while (locked < w->bytes && !atomic_load(&g_stop)) {
            size_t len = w->bytes - locked < chunk ? w->bytes - locked : chunk;
            if (mlock(w->base + locked, len) != 0) {
                w->prep_error = errno;
                break;
            }
            locked += len;
            atomic_store(&w->done, locked);
        }
    }
    w->locked = R.want_lock && locked == w->bytes;
    /* Touch whatever is not locked so that it is really backed by RAM. */
    for (size_t off = locked; off < w->bytes && !atomic_load(&g_stop); off += 4096) {
        ((volatile uint8_t *)w->base)[off] = 0;
        if ((off & (chunk - 1)) == 0)
            atomic_store(&w->done, off);
    }
    atomic_store(&w->done, w->bytes);
}

static void nap(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    if (w->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(w->cpu, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    }
    prepare_region(w);
    atomic_fetch_add(&R.done_count, 1);
    while (!atomic_load(&R.go))
        nap(5);
    if (atomic_load(&R.quit))
        return NULL;
    for (;;) {
        pthread_barrier_wait(&R.bar);
        if (atomic_load(&R.quit))
            break;
        test_run(w, &R.step);
        atomic_fetch_add(&R.done_count, 1);
    }
    return NULL;
}

/* ---- progress & ETA -------------------------------------------------------------------------- */

static double rate_for(int test, double fallback)
{
    return R.rate[test] > 0 ? R.rate[test] : fallback;
}

/* Remaining seconds for the whole run, or -1 if unknown / endless. */
static double estimate_eta(int cycle, int step, uint64_t done, uint64_t total, double step_secs)
{
    const tm5_config_t *cfg = R.cfg;
    int cycles = R.ui.cycles;
    if (cycles == 0)
        return -1;
    double avg = R.test_secs_done > 0.25 ? R.test_bytes_done / R.test_secs_done : 0;
    double cur = step_secs > 1 && done > 0 ? (double)done / step_secs : 0;
    double rate_now = cur > 0 ? cur : rate_for(cfg->seq[step], avg);
    if (rate_now <= 0)
        return -1;
    double eta = (double)(total > done ? total - done : 0) / rate_now;
    for (int c = cycle; c <= cycles; c++) {
        for (int s = (c == cycle ? step + 1 : 0); s < cfg->nseq; s++) {
            double r = rate_for(cfg->seq[s], avg > 0 ? avg : rate_now);
            eta += (double)R.step_bytes[s] / r;
        }
    }
    return eta;
}

static void emit_progress(double now, double eta, double speed)
{
    pthread_mutex_lock(&R.out);
    if (R.json) {
        jo_begin(stdout, "progress");
        jo_dbl(stdout, "elapsed", now - R.t0);
        if (eta >= 0)
            jo_dbl(stdout, "eta", eta);
        else
            jo_null(stdout, "eta");
        jo_dbl(stdout, "run_pct", R.ui.run_pct);
        jo_dbl(stdout, "test_pct", R.ui.test_pct);
        jo_dbl(stdout, "speed", speed);
        jo_u64(stdout, "errors", atomic_load(&R.errors));
        jo_int(stdout, "cycle", R.step.cycle);
        jo_int(stdout, "step", R.step.step);
        fputs(",\"threads\":[", stdout);
        for (int i = 0; i < R.nthreads; i++) {
            uint64_t d = atomic_load_explicit(&R.w[i].done, memory_order_relaxed);
            double pct = R.w[i].total ? 100.0 * (double)d / (double)R.w[i].total : 0;
            fprintf(stdout, "%s%.1f", i ? "," : "", pct > 100 ? 100 : pct);
        }
        fputs("],\"thread_errors\":[", stdout);
        for (int i = 0; i < R.nthreads; i++)
            fprintf(stdout, "%s%llu", i ? "," : "", (unsigned long long)atomic_load(&R.w[i].errors));
        fputc(']', stdout);
        jo_end(stdout);
    } else if (!R.opt->quiet) {
        R.ui.elapsed = now - R.t0;
        R.ui.eta = eta;
        R.ui.speed = speed;
        R.ui.errors = atomic_load(&R.errors);
        ui_draw(&R.ui);
    }
    pthread_mutex_unlock(&R.out);
}

/* ---- memory setup ---------------------------------------------------------------------------- */

static int setup_memory(const sysinfo_t *si)
{
    const options_t *opt = R.opt;
    int cpus[MAX_THREADS];
    int ncpu = cpu_list(cpus, MAX_THREADS);

    int n = opt->threads > 0 ? opt->threads : R.cfg->cores > 0 ? R.cfg->cores : ncpu;
    if (n > MAX_THREADS)
        n = MAX_THREADS;
    if (n < 1)
        n = 1;

    uint64_t avail = si->mem_available;
    uint64_t total;
    if (opt->mem_bytes)
        total = opt->mem_bytes;
    else if (opt->mem_pct > 0)
        total = (uint64_t)((double)avail * opt->mem_pct / 100.0);
    else
        total = sysinfo_default_bytes(si, R.cfg->reserved_mb);

    uint64_t hard_cap = avail > (256ull << 20) ? avail - (256ull << 20) : avail / 2;
    if (total > hard_cap && !opt->force) {
        char a[32], b[32];
        fmt_bytes(total, a, sizeof a);
        fmt_bytes(hard_cap, b, sizeof b);
        say(LV_WARN, "Requested %s but only %s can be used safely right now - testing %s instead (use --force to override).",
            a, b, b);
        total = hard_cap;
    }

    size_t per = (size_t)(total / (uint64_t)n);
    if (per < (4u << 20)) {
        int fit = (int)(total / (4u << 20));
        if (fit < 1)
            fit = 1;
        if (fit < n) {
            say(LV_WARN, "Not enough memory for %d threads - using %d threads instead.", n, fit);
            n = fit;
        }
        per = (size_t)(total / (uint64_t)n);
    }
    size_t align = per >= (64u << 20) ? (2u << 20) : (64u << 10);
    per &= ~(align - 1);
    if (per < (1u << 20)) {
        say(LV_ERR, "Not enough free memory to run a test (need at least 1 MiB per thread).");
        return -1;
    }

    R.nthreads = n;
    R.per = per;
    R.total = (uint64_t)per * (uint64_t)n;
    R.map_len = (size_t)R.total + (2u << 20);
    R.map = mem_map(R.map_len, !opt->no_huge);
    if (!R.map) {
        say(LV_ERR, "Could not reserve %llu MiB of address space: %s", (unsigned long long)(R.total >> 20), strerror(errno));
        return -1;
    }
    uint8_t *base = (uint8_t *)(((uintptr_t)R.map + (2u << 20) - 1) & ~(uintptr_t)((2u << 20) - 1));

    R.w = aligned_alloc(64, sizeof(worker_t) * (size_t)n);
    R.tid = calloc((size_t)n, sizeof(pthread_t));
    if (!R.w || !R.tid) {
        say(LV_ERR, "Out of memory.");
        return -1;
    }
    memset(R.w, 0, sizeof(worker_t) * (size_t)n);
    uint64_t seed = opt->seed;
    for (int i = 0; i < n; i++) {
        worker_t *w = &R.w[i];
        w->id = i;
        w->cpu = n <= ncpu ? cpus[i] : -1;
        w->base = base + (size_t)i * per;
        w->bytes = per;
        w->gline0 = (uint64_t)i * per / LINE_BYTES;
        w->rng = mix64(seed + (uint64_t)i * 0x9e3779b97f4a7c15ULL);
    }

    R.want_lock = !opt->no_lock && si->can_lock;
    if (!opt->no_lock && !si->can_lock)
        say(LV_WARN, "Memory cannot be locked (%s). The kernel may move tested memory to swap or zram, which can hide "
            "errors. Fix: run 'sudo setcap cap_ipc_lock=ep %s' or start the test as root.", si->lock_reason,
            "$(command -v tm5linux-cli)");
    return 0;
}

/* ---- the run ----------------------------------------------------------------------------------- */

static void open_log(const tm5_config_t *cfg, const sysinfo_t *si)
{
    const options_t *opt = R.opt;
    char path[2048];
    if (opt->no_log)
        return;
    if (opt->log_path) {
        str_copy(path, sizeof path, opt->log_path);
    } else {
        if (R.json)
            return;             /* the GUI writes its own log */
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        char dir[1536];
        if (xdg && *xdg == '/')
            snprintf(dir, sizeof dir, "%s/tm5linux/logs", xdg);
        else if (home && *home)
            snprintf(dir, sizeof dir, "%s/.local/share/tm5linux/logs", home);
        else
            return;
        if (mkdir_p(dir, 0755) != 0)
            return;
        char stamp[32];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        snprintf(path, sizeof path, "%s/tm5linux-%s.log", dir, stamp);
    }
    R.logf = fopen(path, "w");
    if (!R.logf) {
        say(LV_WARN, "Cannot write log file %s: %s", path, strerror(errno));
        return;
    }
    char mt[32], ma[32];
    fmt_bytes(si->mem_total, mt, sizeof mt);
    fmt_bytes(si->mem_available, ma, sizeof ma);
    fprintf(R.logf, "TM5 for Linux %s - memory stability test log\n", TM5L_VERSION);
    fprintf(R.logf, "Config : %s by %s (%s)\n", cfg->name, cfg->author, cfg->source);
    fprintf(R.logf, "CPU    : %s (%d threads available)\n", si->cpu, si->cpus);
    fprintf(R.logf, "Memory : %s total, %s available\n", mt, ma);
    fprintf(R.logf, "Kernel : %s %s, transparent hugepages: %s\n\n", si->kernel, si->arch, si->thp);
    fflush(R.logf);
    if (!R.json && !opt->quiet)
        say(LV_INFO, "Log file: %s", path);
}

int runner_run(const tm5_config_t *cfg, const options_t *opt, const sysinfo_t *si, run_result_t *res)
{
    memset(&R, 0, sizeof R);
    memset(res, 0, sizeof *res);
    R.cfg = cfg;
    R.opt = opt;
    R.json = opt->json;
    R.tty = !opt->json && isatty(STDOUT_FILENO);
    pthread_mutex_init(&R.out, NULL);
    atomic_store(&g_stop, 0);
    g_inject = opt->inject;
    g_inject_rate = opt->inject_rate > 1 ? opt->inject_rate : 1;
    atomic_store(&g_injected, 0);
    memset(R.marks, '.', sizeof R.marks - 1);

    if (!opt->json) {
        const char *term = getenv("TERM");
        bool tty = R.tty && term && strcmp(term, "dumb") != 0;
        R.tty = tty;
        ui_init(tty, tty && !getenv("NO_COLOR"));
    }
    open_log(cfg, si);
    for (int i = 0; i < cfg->nwarn; i++)
        say(LV_WARN, "%s", cfg->warn[i]);

    const int cycles = opt->cycles >= 0 ? opt->cycles : cfg->cycles;
    const int scale = opt->time_scale > 0 ? opt->time_scale : 100;
    const unsigned idle_ms = opt->refresh_idle_ms ? opt->refresh_idle_ms : REFRESH_IDLE_MS;

    R.t0 = now_sec();
    if (setup_memory(si) != 0) {
        res->result = RESULT_ERROR;
        goto out_nothreads;
    }

    snprintf(R.ui.title, sizeof R.ui.title, "%s by %s", cfg->name, cfg->author);
    R.ui.cycles = cycles;
    R.ui.nseq = cfg->nseq;
    R.ui.seq = cfg->seq;
    R.ui.marks = R.marks;
    R.ui.mem_bytes = R.total;
    R.ui.threads = R.nthreads;
    R.ui.eta = -1;
    for (int s = 0; s < cfg->nseq; s++) {
        const tm5_test_t *t = &cfg->tests[cfg->seq[s]];
        R.step_bytes[s] = test_step_bytes(t, R.per, test_passes(cfg, t, scale), idle_ms) * (uint64_t)R.nthreads;
    }

    char memtxt[32];
    fmt_bytes(R.total, memtxt, sizeof memtxt);
    if (R.json) {
        pthread_mutex_lock(&R.out);
        jo_config(stdout, cfg, scale, idle_ms, cycles);
        jo_begin(stdout, "alloc_start");
        jo_u64(stdout, "bytes", R.total);
        jo_u64(stdout, "per_thread", R.per);
        jo_int(stdout, "threads", R.nthreads);
        jo_bool(stdout, "lock", R.want_lock);
        jo_int(stdout, "cycles", cycles);
        jo_end(stdout);
        pthread_mutex_unlock(&R.out);
    }
    human(LV_INFO, "Starting %s by %s: %s on %d threads, %d cycle%s of %d steps.", cfg->name, cfg->author, memtxt,
          R.nthreads, cycles, cycles == 1 ? "" : "s", cfg->nseq);
    human(LV_INFO, "Preparing memory - this can take a few seconds...");

    /* Start workers; they first allocate (and lock) their memory. */
    pthread_barrier_init(&R.bar, NULL, (unsigned)R.nthreads + 1);
    int started = 0;
    for (int i = 0; i < R.nthreads; i++) {
        if (pthread_create(&R.tid[i], NULL, worker_main, &R.w[i]) != 0) {
            say(LV_ERR, "Could not start thread %d: %s", i, strerror(errno));
            break;
        }
        started++;
    }
    if (started < R.nthreads) {
        /* Let the started threads finish preparing, then send them home. */
        atomic_store(&g_stop, 1);
        atomic_store(&R.quit, 1);
        atomic_store(&R.go, 1);
        for (int i = 0; i < started; i++)
            pthread_join(R.tid[i], NULL);
        pthread_barrier_destroy(&R.bar);
        R.nthreads = started;
        res->result = RESULT_ERROR;
        goto out_nothreads;
    }

    R.ui.phase = PH_PREPARE;
    double last_emit = 0;
    while (atomic_load(&R.done_count) < R.nthreads) {
        nap(50);
        double now = now_sec();
        if (now - last_emit >= (R.json ? 0.25 : 0.5)) {
            last_emit = now;
            uint64_t d = 0;
            for (int i = 0; i < R.nthreads; i++)
                d += atomic_load(&R.w[i].done);
            R.ui.prep_pct = 100.0 * (double)d / (double)R.total;
            if (R.json) {
                pthread_mutex_lock(&R.out);
                jo_begin(stdout, "alloc_progress");
                jo_dbl(stdout, "pct", R.ui.prep_pct);
                jo_end(stdout);
                pthread_mutex_unlock(&R.out);
            } else {
                emit_progress(now, -1, 0);
            }
        }
    }

    bool all_locked = true;
    int lock_err = 0;
    for (int i = 0; i < R.nthreads; i++) {
        all_locked &= R.w[i].locked;
        if (R.w[i].prep_error)
            lock_err = R.w[i].prep_error;
    }
    double huge = 100.0 * (double)mem_anon_huge_bytes() / (double)R.total;
    if (huge > 100)
        huge = 100;
    R.ui.locked = all_locked;
    R.ui.huge_pct = huge;
    atomic_store(&R.go, 1);
    double prep_secs = now_sec() - R.t0;
    if (R.json) {
        pthread_mutex_lock(&R.out);
        jo_begin(stdout, "alloc_done");
        jo_u64(stdout, "bytes", R.total);
        jo_int(stdout, "threads", R.nthreads);
        jo_bool(stdout, "locked", all_locked);
        jo_dbl(stdout, "huge_pct", huge);
        jo_dbl(stdout, "seconds", prep_secs);
        jo_end(stdout);
        pthread_mutex_unlock(&R.out);
    }
    if (R.want_lock && !all_locked && !atomic_load(&g_stop))
        say(LV_WARN, "Locking memory failed (%s) - testing continues, but some memory may be swapped out.",
            strerror(lock_err ? lock_err : ENOMEM));
    human(all_locked ? LV_OK : LV_WARN, "Memory ready in %.1f s: %s %s, %.0f %% in huge pages.", prep_secs, memtxt,
          all_locked ? "locked in RAM" : "NOT locked", huge);

    /* ---- test loop ---- */
    R.ui.phase = PH_TESTING;
    uint64_t run_seed = opt->seed_set ? opt->seed : mix64((uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32));
    double run_bytes = 0;                                   /* bytes of the whole run */
    for (int s = 0; s < cfg->nseq; s++)
        run_bytes += (double)R.step_bytes[s] * cycles;
    double speed = 0, speed_t = now_sec();
    uint64_t cum_bytes = 0, speed_b = 0;                    /* bytes moved by finished steps */
    int cycle = 1;
    for (; cycles == 0 || cycle <= cycles; cycle++) {
        if (atomic_load(&g_stop))
            break;
        memset(R.marks, '.', (size_t)cfg->nseq);
        uint64_t cycle_errors_start = atomic_load(&R.errors);
        for (int s = 0; s < cfg->nseq && !atomic_load(&g_stop); s++) {
            const int ti = cfg->seq[s];
            const tm5_test_t *t = &cfg->tests[ti];
            step_t st = { .index = ti, .t = t, .cycle = cycle, .step = s, .passes = test_passes(cfg, t, scale),
                          .seed = run_seed, .refresh_idle_ms = idle_ms };
            R.step = st;
            uint64_t step_total = 0;
            for (int i = 0; i < R.nthreads; i++) {
                atomic_store(&R.w[i].done, 0);
                R.w[i].total = test_step_bytes(t, R.per, st.passes, idle_ms);
                step_total += R.w[i].total;
            }
            atomic_store(&R.step_errors, 0);
            atomic_store(&R.done_count, 0);

            R.ui.cycle = cycle;
            R.ui.step = s;
            R.ui.test_index = ti;
            R.ui.test_pct = 0;
            str_copy(R.ui.func, sizeof R.ui.func, func_name(t->func));
            test_summary(t, R.ui.summary, sizeof R.ui.summary);
            test_explain(t, idle_ms, R.ui.explain, sizeof R.ui.explain);
            R.marks[s] = '>';

            pthread_mutex_lock(&R.out);
            if (R.json) {
                jo_begin(stdout, "test_start");
                jo_int(stdout, "cycle", cycle);
                jo_int(stdout, "cycles", cycles);
                jo_int(stdout, "step", s);
                jo_int(stdout, "steps", cfg->nseq);
                jo_int(stdout, "test", ti);
                jo_str(stdout, "function", func_name(t->func));
                jo_int(stdout, "passes", st.passes);
                jo_u64(stdout, "bytes", step_total);
                jo_str(stdout, "summary", R.ui.summary);
                jo_str(stdout, "explain", R.ui.explain);
                jo_end(stdout);
            }
            if (!R.json && !opt->quiet) {
                char ev[256];
                snprintf(ev, sizeof ev, "Step %d/%d: test #%d %s started.", s + 1, cfg->nseq, ti, func_name(t->func));
                ui_event(LV_INFO, ev);
            }
            {
                char lg[512];
                snprintf(lg, sizeof lg, "Cycle %d, step %d/%d: test #%d %s started (%s, %d passes).", cycle, s + 1,
                         cfg->nseq, ti, func_name(t->func), R.ui.summary, st.passes);
                logline("INFO", lg);
            }
            pthread_mutex_unlock(&R.out);

            const double ts0 = now_sec();
            last_emit = 0;
            pthread_barrier_wait(&R.bar);                       /* go! */

            while (atomic_load(&R.done_count) < R.nthreads) {
                nap(50);
                double now = now_sec();
                if (now - last_emit < (R.json ? 0.25 : 0.5))
                    continue;
                last_emit = now;
                uint64_t done = 0;
                for (int i = 0; i < R.nthreads; i++)
                    done += atomic_load_explicit(&R.w[i].done, memory_order_relaxed);
                if (now - speed_t >= 1.0) {
                    double inst = (double)(cum_bytes + done - speed_b) / (now - speed_t);
                    speed = speed > 0 ? 0.6 * speed + 0.4 * inst : inst;
                    speed_t = now;
                    speed_b = cum_bytes + done;
                }
                R.ui.test_pct = step_total ? 100.0 * (double)done / (double)step_total : 0;
                double eta = estimate_eta(cycle, s, done, step_total, now - ts0);
                double testing = R.test_secs_done + (now - ts0);
                if (eta >= 0)
                    R.ui.run_pct = 100.0 * testing / (testing + eta);
                else if (cycles > 0 && run_bytes > 0)
                    R.ui.run_pct = 100.0 * (double)(cum_bytes + done) / run_bytes;
                R.ui.eta = eta;
                emit_progress(now, eta, t->func == FN_REFRESH ? 0 : speed);
            }

            const double secs = now_sec() - ts0;
            const uint64_t serr = atomic_load(&R.step_errors);
            for (int i = 0; i < R.nthreads; i++)
                cum_bytes += atomic_load(&R.w[i].done);
            if (!atomic_load(&g_stop) || serr) {
                double r = secs > 0 ? (double)step_total / secs : 0;
                R.rate[ti] = R.rate[ti] > 0 ? 0.5 * R.rate[ti] + 0.5 * r : r;
                R.test_bytes_done += (double)step_total;
                R.test_secs_done += secs;
            }
            R.marks[s] = serr ? 'x' : (atomic_load(&g_stop) ? '.' : '+');

            char dur[32];
            if (secs < 60)
                snprintf(dur, sizeof dur, "%.1f s", secs);
            else
                fmt_duration(secs, dur, sizeof dur);
            pthread_mutex_lock(&R.out);
            if (R.json) {
                jo_begin(stdout, "test_end");
                jo_int(stdout, "cycle", cycle);
                jo_int(stdout, "step", s);
                jo_int(stdout, "test", ti);
                jo_u64(stdout, "errors", serr);
                jo_dbl(stdout, "seconds", secs);
                jo_bool(stdout, "completed", !atomic_load(&g_stop));
                jo_end(stdout);
            }
            if (serr)
                human(LV_ERR, "Test #%d found %llu error%s (took %s).", ti, (unsigned long long)serr, serr == 1 ? "" : "s", dur);
            else if (!atomic_load(&g_stop))
                human(LV_OK, "Test #%d passed in %s.", ti, dur);
            pthread_mutex_unlock(&R.out);
        }
        if (atomic_load(&g_stop))
            break;
        uint64_t cerr = atomic_load(&R.errors) - cycle_errors_start;
        res->cycles_done = cycle;
        pthread_mutex_lock(&R.out);
        if (R.json) {
            jo_begin(stdout, "cycle_end");
            jo_int(stdout, "cycle", cycle);
            jo_u64(stdout, "errors", cerr);
            jo_u64(stdout, "total_errors", atomic_load(&R.errors));
            jo_end(stdout);
        }
        if (cycles)
            human(atomic_load(&R.errors) ? LV_ERR : LV_OK, "Cycle %d of %d finished - %llu error%s so far.", cycle, cycles,
                  (unsigned long long)atomic_load(&R.errors), atomic_load(&R.errors) == 1 ? "" : "s");
        else
            human(atomic_load(&R.errors) ? LV_ERR : LV_OK, "Cycle %d finished - %llu error%s so far.", cycle,
                  (unsigned long long)atomic_load(&R.errors), atomic_load(&R.errors) == 1 ? "" : "s");
        pthread_mutex_unlock(&R.out);
    }

    /* Release and join the workers. */
    atomic_store(&R.quit, 1);
    pthread_barrier_wait(&R.bar);
    for (int i = 0; i < R.nthreads; i++)
        pthread_join(R.tid[i], NULL);
    pthread_barrier_destroy(&R.bar);

    res->errors = atomic_load(&R.errors);
    res->injected = atomic_load(&g_injected);
    if (res->errors)
        res->result = RESULT_FAIL;
    else if (atomic_load(&g_stop))
        res->result = RESULT_STOPPED;
    else
        res->result = RESULT_PASS;

out_nothreads:
    res->elapsed = now_sec() - R.t0;
    if (!R.json && !opt->quiet)
        ui_end();

    static const char *rname[] = { "pass", "fail", "stopped", "error" };
    char dur[32], mem[32];
    fmt_duration(res->elapsed, dur, sizeof dur);
    fmt_bytes(R.total, mem, sizeof mem);

    if (R.json) {
        jo_begin(stdout, "finish");
        jo_str(stdout, "result", rname[res->result]);
        jo_u64(stdout, "errors", res->errors);
        jo_dbl(stdout, "elapsed", res->elapsed);
        jo_int(stdout, "cycles_done", res->cycles_done);
        fputs(",\"errors_by_test\":{", stdout);
        bool first = true;
        for (int i = 0; i < MAX_TESTS; i++) {
            uint64_t e = atomic_load(&R.test_errors[i]);
            if (!e)
                continue;
            fprintf(stdout, "%s\"%d\":%llu", first ? "" : ",", i, (unsigned long long)e);
            first = false;
        }
        fputc('}', stdout);
        jo_end(stdout);
    }

    /* Final summary for humans (terminal and log file). */
    char by_test[512] = "";
    size_t bl = 0;
    for (int i = 0; i < MAX_TESTS; i++) {
        uint64_t e = atomic_load(&R.test_errors[i]);
        if (e && bl < sizeof by_test - 32)
            bl += (size_t)snprintf(by_test + bl, sizeof by_test - bl, "%s#%d x%llu", bl ? ", " : "", i,
                                   (unsigned long long)e);
    }
    char line1[512], line2[512];
    switch (res->result) {
    case RESULT_PASS:
        snprintf(line1, sizeof line1, "PASSED - no errors in %d cycle%s (%s)", res->cycles_done,
                 res->cycles_done == 1 ? "" : "s", dur);
        snprintf(line2, sizeof line2, "Your memory passed %s by %s.", cfg->name, cfg->author);
        break;
    case RESULT_FAIL:
        snprintf(line1, sizeof line1, "FAILED - %llu error%s found (%s)", (unsigned long long)res->errors,
                 res->errors == 1 ? "" : "s", dur);
        snprintf(line2, sizeof line2,
                 "Errors by test: %s. Your memory is NOT stable at the current settings: loosen timings, lower the "
                 "frequency or adjust voltages (at stock settings, test one module at a time).", by_test);
        break;
    case RESULT_STOPPED:
        snprintf(line1, sizeof line1, "STOPPED after %s - no errors so far, but the test did not finish", dur);
        snprintf(line2, sizeof line2, "Completed %d of %d cycles. A stopped test does not prove stability.",
                 res->cycles_done, cycles);
        break;
    default:
        snprintf(line1, sizeof line1, "The test could not run");
        snprintf(line2, sizeof line2, "See the messages above for the reason.");
    }
    if (R.logf) {
        fprintf(R.logf, "\nRESULT : %s\n         %s\nMemory : %s on %d threads\n", line1, line2, mem, R.nthreads);
        fclose(R.logf);
        R.logf = NULL;
    }
    if (!R.json && !opt->silent) {
        bool c = ui_color();
        const char *col = res->result == RESULT_PASS ? "\033[1;92m" : res->result == RESULT_FAIL ? "\033[1;91m" : "\033[1;93m";
        const char *icon = res->result == RESULT_PASS ? (ui_utf8() ? "✔" : "OK") : res->result == RESULT_FAIL ? (ui_utf8() ? "✘" : "XX") : (ui_utf8() ? "■" : "--");
        printf("\n  %s%s %s%s\n", c ? col : "", icon, line1, c ? "\033[0m" : "");
        printf("    %s\n", line2);
        if (R.total)
            printf("    %s by %s · %s on %d threads%s\n", cfg->name, cfg->author, mem, R.nthreads,
                   R.ui.locked ? " · memory locked" : "");
        printf("\n");
        fflush(stdout);
    }

    if (R.map)
        mem_unmap(R.map, R.map_len);
    free(R.w);
    free(R.tid);
    R.w = NULL;
    R.tid = NULL;
    R.map = NULL;
    pthread_mutex_destroy(&R.out);
    return res->result == RESULT_ERROR ? -1 : 0;
}
