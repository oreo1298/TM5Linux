/* TM5 for Linux - command line front end of the memory test engine.
 * SPDX-License-Identifier: MIT
 */
#include "config.h"
#include "jsonout.h"
#include "runner.h"
#include "sysinfo.h"
#include "ui.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static volatile sig_atomic_t g_signals;

static void on_signal(int sig)
{
    (void)sig;
    if (g_signals++ == 0) {
        runner_request_stop();
        return;
    }
    ui_restore_terminal_async();
    _exit(130);
}

/* With --stdin-control the GUI stops the test by writing "stop" or by closing the pipe. This also
 * works when the engine runs with higher privileges (pkexec) and cannot be signalled. */
static void *stdin_watch(void *arg)
{
    (void)arg;
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        if (strncmp(line, "stop", 4) == 0)
            break;
    }
    runner_request_stop();
    return NULL;
}

static void usage(FILE *f)
{
    fprintf(f,
        "TM5 for Linux %s - RAM stability tester that runs TestMem5 (TM5) configs\n"
        "\n"
        "Usage: tm5linux-cli [options]\n"
        "\n"
        "Test options:\n"
        "  -c, --config NAME|FILE   preset name (e.g. extreme, absolut) or TM5 .cfg file\n"
        "                           (default: extreme = Extreme1 @anta777)\n"
        "  -m, --memory SIZE        how much RAM to test: 12G, 8192M, 90%% (of available)\n"
        "                           default: all available memory minus a safety reserve\n"
        "  -t, --threads N          worker threads (default: all CPU threads)\n"
        "  -n, --cycles N           number of cycles (default: from the config, 0 = forever)\n"
        "  -s, --time-scale PCT     scale the duration of every test (default 100)\n"
        "  -e, --stop-on-error      stop at the first error\n"
        "      --no-lock            do not lock the tested memory in RAM\n"
        "      --no-hugepages       do not ask for transparent huge pages\n"
        "      --force              allow testing more memory than is currently available\n"
        "      --seed N             fixed random seed (to repeat a run exactly)\n"
        "      --log FILE           write the log to FILE\n"
        "      --no-log             do not write a log file\n"
        "\n"
        "Information:\n"
        "  -l, --list-presets       list the available configs\n"
        "  -d, --describe           explain what every test of the config does\n"
        "  -i, --sysinfo            show CPU, memory and memory-lock status\n"
        "      --selftest           check that the engine detects simulated bit flips\n"
        "      --inject-faults[=N]  simulate a bit flip in 1 of every N written blocks (testing only)\n"
        "      --json               machine-readable output (used by the GUI)\n"
        "  -q, --quiet              only print the final result\n"
        "  -V, --version            show the version\n"
        "  -h, --help               show this help\n"
        "\n"
        "Exit status: 0 = passed, 1 = memory errors found, 2 = stopped, 3 = could not run.\n"
        "Logs are written to ~/.local/share/tm5linux/logs/. Press Ctrl+C to stop a running test.\n",
        TM5L_VERSION);
}

static bool parse_size(const char *s, options_t *opt)
{
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || v <= 0)
        return false;
    while (*end == ' ')
        end++;
    if (*end == '%') {
        if (v > 100)
            return false;
        opt->mem_pct = v;
        opt->mem_bytes = 0;
        return true;
    }
    double mul = 1024.0 * 1024.0;                  /* plain numbers are MiB, like TM5 */
    switch (toupper((unsigned char)*end)) {
    case 'K': mul = 1024.0; break;
    case 'M': case '\0': mul = 1024.0 * 1024.0; break;
    case 'G': mul = 1024.0 * 1024.0 * 1024.0; break;
    case 'T': mul = 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
    case 'B': mul = 1; break;
    default: return false;
    }
    opt->mem_bytes = (uint64_t)(v * mul);
    opt->mem_pct = 0;
    return opt->mem_bytes >= (1u << 20);
}

static int print_describe(const tm5_config_t *cfg, const options_t *opt)
{
    unsigned idle = opt->refresh_idle_ms ? opt->refresh_idle_ms : REFRESH_IDLE_MS;
    int scale = opt->time_scale > 0 ? opt->time_scale : 100;
    if (opt->json) {
        jo_config(stdout, cfg, scale, idle, opt->cycles);
        return 0;
    }
    int cycles = opt->cycles >= 0 ? opt->cycles : cfg->cycles;
    printf("%s by %s\n%s\n\n", cfg->name, cfg->author, cfg->source);
    printf("Sequence (%d steps per cycle, %d cycle%s%s):", cfg->nseq, cycles, cycles == 1 ? "" : "s",
           cycles == 0 ? " = until stopped" : "");
    for (int i = 0; i < cfg->nseq; i++)
        printf(" %d", cfg->seq[i]);
    printf("\n\n");
    for (int i = 0; i < cfg->ntests; i++) {
        const tm5_test_t *t = &cfg->tests[i];
        if (!t->present)
            continue;
        int uses = 0;
        for (int s = 0; s < cfg->nseq; s++)
            uses += cfg->seq[s] == i;
        char summary[256], explain[1024];
        test_summary(t, summary, sizeof summary);
        test_explain(t, idle, explain, sizeof explain);
        printf("Test #%-2d %-14s %s\n", i, func_name(t->func), summary);
        printf("         %s\n", explain);
        int passes = test_passes(cfg, t, scale);
        if (uses)
            printf("         %d pass%s each time it runs, %d time%s per cycle\n\n", passes, passes == 1 ? "" : "es",
                   uses, uses == 1 ? "" : "s");
        else
            printf("         not used in the test sequence%s\n\n", t->enabled ? "" : " (disabled)");
    }
    for (int i = 0; i < cfg->nwarn; i++)
        printf("Warning: %s\n", cfg->warn[i]);
    return 0;
}

static int print_sysinfo(const sysinfo_t *si, const tm5_config_t *cfg, bool json)
{
    if (json) {
        jo_sysinfo(stdout, si, cfg);
        return 0;
    }
    char a[32], b[32], c[32], d[32];
    fmt_bytes(si->mem_total, a, sizeof a);
    fmt_bytes(si->mem_available, b, sizeof b);
    fmt_bytes(si->swap_total, c, sizeof c);
    fmt_bytes(sysinfo_default_bytes(si, cfg ? cfg->reserved_mb : 128), d, sizeof d);
    printf("TM5 for Linux %s\n\n", TM5L_VERSION);
    printf("CPU               %s (%d threads)\n", si->cpu, si->cpus);
    printf("Kernel            %s %s\n", si->kernel, si->arch);
    printf("Memory            %s total, %s available\n", a, b);
    printf("Swap              %s\n", c);
    printf("Default test size %s\n", d);
    printf("Huge pages (THP)  %s\n", si->thp);
    printf("Memory locking    %s (%s)\n", si->can_lock ? "available" : "NOT available", si->lock_reason);
    if (!si->can_lock)
        printf("\nTo allow locking without root, run:\n  sudo setcap cap_ipc_lock=ep \"$(command -v tm5linux-cli)\"\n");
    return 0;
}

static int print_presets(bool json)
{
    preset_t *list = calloc(128, sizeof *list);
    if (!list)
        return 3;
    int n = presets_list(list, 128);
    if (json) {
        jo_presets(stdout, list, n);
    } else {
        printf("Available configs (use the first column with --config):\n\n");
        for (int i = 0; i < n; i++)
            printf("  %-14s %-24s %2d tests, %2d steps, %2d cycles   %s\n", list[i].alias, list[i].name, list[i].ntests,
                   list[i].nseq, list[i].cycles, list[i].path);
        char udir[1024];
        config_user_dir(udir, sizeof udir);
        printf("\nPut your own TM5 .cfg files in %s\n", udir);
    }
    free(list);
    return 0;
}

static const char selftest_cfg[] =
    "[Main Section]\nConfig Name=Self-test\nConfig Author=tm5linux\nTests=7\nTime (%)=100\nCycles=2\n"
    "Test Sequence=0,1,2,3,4,5,6\n"
    "[Test0]\nFunction=SimpleTest\nPattern Mode=0\nParameter=0\nTest Block Size (Mb)=4\n"
    "[Test1]\nFunction=SimpleTest\nPattern Mode=1\nPattern Param0=0x1E5F\nPattern Param1=0x45357354\nParameter=2\nTest Block Size (Mb)=0\n"
    "[Test2]\nFunction=SimpleTest\nPattern Mode=2\nPattern Param0=0x2305B\nPattern Param1=0x97893FB2\nParameter=256\nTest Block Size (Mb)=4\n"
    "[Test3]\nFunction=MirrorMove\nPattern Mode=0\nParameter=4\nTest Block Size (Mb)=0\n"
    "[Test4]\nFunction=MirrorMove128\nPattern Mode=0\nParameter=2\nTest Block Size (Mb)=0\n"
    "[Test5]\nFunction=RefreshStable\nTime (%)=25\nPattern Mode=0\nParameter=0\nTest Block Size (Mb)=0\n"
    "[Test6]\nFunction=SimpleTest\nPattern Mode=0\nParameter=254\nTest Block Size (Mb)=0\n";

static int selftest(const sysinfo_t *si)
{
    tm5_config_t *cfg = malloc(sizeof *cfg);
    char err[256];
    if (!cfg || config_parse(selftest_cfg, "self-test", cfg, err, sizeof err) != 0) {
        fprintf(stderr, "self-test config error: %s\n", err);
        free(cfg);
        return 3;
    }
    options_t opt = { 0 };
    opt.mem_bytes = 64ull << 20;
    opt.threads = si->cpus < 4 ? si->cpus : 4;
    opt.cycles = -1;
    opt.time_scale = 100;
    opt.quiet = true;
    opt.silent = true;
    opt.no_lock = true;
    opt.no_log = true;
    opt.refresh_idle_ms = 100;
    opt.seed = 12345;
    opt.seed_set = true;

    int failures = 0;
    run_result_t r;

    printf("TM5 for Linux self-test\n\n");
    printf("  1. Clean run (no errors expected) ............ ");
    fflush(stdout);
    runner_run(cfg, &opt, si, &r);
    if (r.result == RESULT_PASS && r.errors == 0) {
        printf("ok (%.1f s)\n", r.elapsed);
    } else {
        printf("FAILED: %llu false errors\n", (unsigned long long)r.errors);
        failures++;
    }

    printf("  2. Simulated bit flips must all be found ..... ");
    fflush(stdout);
    opt.inject = true;
    runner_run(cfg, &opt, si, &r);
    if (r.injected > 0 && r.errors == r.injected) {
        printf("ok (%llu of %llu found)\n", (unsigned long long)r.errors, (unsigned long long)r.injected);
    } else {
        printf("FAILED: %llu of %llu found\n", (unsigned long long)r.errors, (unsigned long long)r.injected);
        failures++;
    }

    printf("  3. Bundled extreme config parses ............. ");
    if (config_resolve("extreme", cfg, err, sizeof err) == 0 && cfg->nseq == 31 && cfg->ntests == 16 &&
        cfg->time_pct == 500 && cfg->cycles == 3) {
        printf("ok (%s by %s, %d steps)\n", cfg->name, cfg->author, cfg->nseq);
    } else {
        printf("FAILED: %s\n", err);
        failures++;
    }
    free(cfg);
    printf("\n%s\n", failures ? "SELF-TEST FAILED" : "Self-test passed.");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    options_t opt = { 0 };
    opt.cycles = -1;
    opt.time_scale = 100;
    bool list = false, describe = false, sysinfo = false, run_selftest = false, stdin_ctl = false;

    enum { O_NOLOCK = 256, O_NOHUGE, O_FORCE, O_SEED, O_LOG, O_NOLOG, O_SELFTEST, O_JSON, O_STDIN, O_INJECT, O_IDLE };
    static const struct option longopts[] = {
        { "config", required_argument, 0, 'c' },     { "memory", required_argument, 0, 'm' },
        { "threads", required_argument, 0, 't' },    { "cycles", required_argument, 0, 'n' },
        { "time-scale", required_argument, 0, 's' }, { "stop-on-error", no_argument, 0, 'e' },
        { "no-lock", no_argument, 0, O_NOLOCK },     { "no-hugepages", no_argument, 0, O_NOHUGE },
        { "force", no_argument, 0, O_FORCE },        { "seed", required_argument, 0, O_SEED },
        { "log", required_argument, 0, O_LOG },      { "no-log", no_argument, 0, O_NOLOG },
        { "list-presets", no_argument, 0, 'l' },     { "describe", no_argument, 0, 'd' },
        { "sysinfo", no_argument, 0, 'i' },          { "selftest", no_argument, 0, O_SELFTEST },
        { "json", no_argument, 0, O_JSON },          { "stdin-control", no_argument, 0, O_STDIN },
        { "inject-faults", optional_argument, 0, O_INJECT }, { "refresh-idle-ms", required_argument, 0, O_IDLE },
        { "quiet", no_argument, 0, 'q' },            { "version", no_argument, 0, 'V' },
        { "help", no_argument, 0, 'h' },             { 0, 0, 0, 0 },
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:m:t:n:s:eldiqVh", longopts, NULL)) != -1) {
        switch (c) {
        case 'c': opt.config = optarg; break;
        case 'm':
            if (!parse_size(optarg, &opt)) {
                fprintf(stderr, "tm5linux-cli: invalid memory size '%s' (examples: 12G, 8192M, 90%%)\n", optarg);
                return 3;
            }
            break;
        case 't': opt.threads = atoi(optarg); break;
        case 'n': opt.cycles = atoi(optarg); break;
        case 's':
            opt.time_scale = atoi(optarg);
            if (opt.time_scale < 1 || opt.time_scale > 100000) {
                fprintf(stderr, "tm5linux-cli: --time-scale must be between 1 and 100000\n");
                return 3;
            }
            break;
        case 'e': opt.stop_on_error = true; break;
        case O_NOLOCK: opt.no_lock = true; break;
        case O_NOHUGE: opt.no_huge = true; break;
        case O_FORCE: opt.force = true; break;
        case O_SEED: opt.seed = strtoull(optarg, NULL, 0); opt.seed_set = true; break;
        case O_LOG: opt.log_path = optarg; break;
        case O_NOLOG: opt.no_log = true; break;
        case 'l': list = true; break;
        case 'd': describe = true; break;
        case 'i': sysinfo = true; break;
        case O_SELFTEST: run_selftest = true; break;
        case O_JSON: opt.json = true; break;
        case O_STDIN: stdin_ctl = true; break;
        case O_INJECT:
            opt.inject = true;
            opt.inject_rate = optarg ? (unsigned)strtoul(optarg, NULL, 10) : 1;
            break;
        case O_IDLE: opt.refresh_idle_ms = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'q': opt.quiet = true; break;
        case 'V': printf("tm5linux-cli %s\n", TM5L_VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 3;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "tm5linux-cli: unexpected argument '%s' (see --help)\n", argv[optind]);
        return 3;
    }

    if (list)
        return print_presets(opt.json);

    sysinfo_t si;
    sysinfo_read(&si);
    if (run_selftest)
        return selftest(&si);

    tm5_config_t *cfg = malloc(sizeof *cfg);
    char err[512];
    if (!cfg)
        return 3;
    if (config_resolve(opt.config, cfg, err, sizeof err) != 0) {
        if (sysinfo) {
            int rc = print_sysinfo(&si, NULL, opt.json);
            free(cfg);
            return rc;
        }
        if (opt.json) {
            jo_begin(stdout, "finish");
            jo_str(stdout, "result", "error");
            jo_str(stdout, "message", err);
            jo_end(stdout);
        }
        fprintf(stderr, "tm5linux-cli: %s\n", err);
        free(cfg);
        return 3;
    }
    if (sysinfo || describe) {
        if (sysinfo)
            print_sysinfo(&si, cfg, opt.json);
        if (describe)
            print_describe(cfg, &opt);
        free(cfg);
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (opt.json) {
        jo_begin(stdout, "hello");
        jo_str(stdout, "version", TM5L_VERSION);
        jo_int(stdout, "pid", (long long)getpid());
        jo_end(stdout);
        jo_sysinfo(stdout, &si, cfg);
    }
    if (stdin_ctl) {
        pthread_t th;
        if (pthread_create(&th, NULL, stdin_watch, NULL) == 0)
            pthread_detach(th);
    }

    run_result_t res;
    runner_run(cfg, &opt, &si, &res);
    free(cfg);
    switch (res.result) {
    case RESULT_PASS:    return 0;
    case RESULT_FAIL:    return 1;
    case RESULT_STOPPED: return 2;
    default:             return 3;
    }
}
