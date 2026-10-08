/* TM5 for Linux - terminal user interface (used when not in --json mode).
 * SPDX-License-Identifier: MIT
 *
 * On a terminal a live dashboard is drawn on the alternate screen; when the output is redirected
 * to a file or pipe, plain timestamped lines are printed instead.
 */
#include "ui.h"
#include "tm5.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_EVENTS 7

static bool   g_tty, g_color, g_alt, g_utf8;
static double g_last_plain;
static int    g_nev;
static struct {
    char when[12];
    int  level;
    char text[320];
} g_ev[MAX_EVENTS];

/* ---- tiny string builder ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len, cap;
} sb_t;

static void sb_add(sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (sb->len + (size_t)n < sb->cap) {
            sb->len += (size_t)n;
            return;
        }
        size_t ncap = (sb->cap + (size_t)n + 1) * 2;
        char *nb = realloc(sb->buf, ncap);
        if (!nb)
            return;
        sb->buf = nb;
        sb->cap = ncap;
    }
}

/* ---- colours -------------------------------------------------------------------------------- */

#define RST  (g_color ? "\033[0m" : "")
#define BOLD (g_color ? "\033[1m" : "")
#define DIM  (g_color ? "\033[90m" : "")
#define CYAN (g_color ? "\033[96m" : "")
#define GRN  (g_color ? "\033[92m" : "")
#define YEL  (g_color ? "\033[93m" : "")
#define RED  (g_color ? "\033[91m" : "")
#define WHT  (g_color ? "\033[97m" : "")
#define INV  (g_color ? "\033[7m" : "")

static const char *lv_color(int lv)
{
    switch (lv) {
    case LV_OK:   return GRN;
    case LV_WARN: return YEL;
    case LV_ERR:  return RED;
    default:      return WHT;
    }
}

static const char *lv_icon(int lv)
{
    if (g_utf8) {
        switch (lv) {
        case LV_OK:   return "✔";
        case LV_WARN: return "▲";
        case LV_ERR:  return "✘";
        default:      return "•";
        }
    }
    switch (lv) {
    case LV_OK:   return "+";
    case LV_WARN: return "!";
    case LV_ERR:  return "x";
    default:      return "-";
    }
}

static bool locale_is_utf8(void)
{
    const char *vars[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    for (int i = 0; i < 3; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v)
            return strstr(v, "UTF-8") || strstr(v, "utf8") || strstr(v, "UTF8") || strstr(v, "utf-8");
    }
    return false;
}

bool ui_color(void) { return g_color; }
bool ui_utf8(void) { return g_utf8; }

void ui_init(bool tty, bool color)
{
    g_tty = tty;
    g_color = color;
    g_utf8 = locale_is_utf8();
    g_last_plain = now_sec();
}

void ui_event(int level, const char *text)
{
    char when[12];
    fmt_clock(when, sizeof when);
    if (!g_tty) {
        printf("[%s] %s %s\n", when, lv_icon(level), text);
        fflush(stdout);
        return;
    }
    if (g_nev == MAX_EVENTS) {
        memmove(&g_ev[0], &g_ev[1], sizeof g_ev[0] * (MAX_EVENTS - 1));
        g_nev--;
    }
    str_copy(g_ev[g_nev].when, sizeof g_ev[0].when, when);
    str_copy(g_ev[g_nev].text, sizeof g_ev[0].text, text);
    g_ev[g_nev].level = level;
    g_nev++;
}

static int term_width(void)
{
    struct winsize ws;
    int w = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        w = ws.ws_col;
    if (w > 110)
        w = 110;
    if (w < 50)
        w = 50;
    return w;
}

static void bar(sb_t *sb, double pct, int width, const char *color)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    int fill = (int)(pct / 100.0 * width + 0.5);
    sb_add(sb, "%s", color);
    for (int i = 0; i < fill; i++)
        sb_add(sb, "%s", g_utf8 ? "█" : "#");
    sb_add(sb, "%s", DIM);
    for (int i = fill; i < width; i++)
        sb_add(sb, "%s", g_utf8 ? "░" : "-");
    sb_add(sb, "%s %s%3.0f %%%s", RST, BOLD, pct, RST);
}

/* Word-wrap text into at most max_lines lines of width cols, each prefixed with indent. */
static void wrap(sb_t *sb, const char *text, int cols, int indent, int max_lines)
{
    const char *p = text;
    for (int line = 0; *p && line < max_lines; line++) {
        while (*p == ' ')
            p++;
        int len = (int)strlen(p);
        int take = len;
        if (len > cols) {
            take = cols;
            while (take > 0 && p[take] != ' ')
                take--;
            if (take == 0)
                take = cols;
        }
        bool last = (line == max_lines - 1) && len > take;
        sb_add(sb, "%*s%s%.*s%s%s\033[K\n", indent, "", DIM, take - (last ? 3 : 0), p, last ? "..." : "", RST);
        p += take;
    }
}

void ui_draw(const ui_state_t *s)
{
    char el[32], eta[32], mem[32];
    fmt_duration(s->elapsed, el, sizeof el);
    if (s->eta >= 0)
        fmt_duration(s->eta, eta, sizeof eta);
    else
        str_copy(eta, sizeof eta, s->cycles == 0 ? "until stopped" : "estimating...");
    fmt_bytes(s->mem_bytes, mem, sizeof mem);

    if (!g_tty) {
        double now = now_sec();
        if (s->phase != PH_TESTING || now - g_last_plain < 30.0)
            return;
        g_last_plain = now;
        printf("[%s] status: cycle %d/%d, step %d/%d (test #%d), overall %.0f%%, this test %.0f%%, %llu errors, %s left\n",
               el, s->cycle, s->cycles, s->step + 1, s->nseq, s->test_index, s->run_pct, s->test_pct,
               (unsigned long long)s->errors, eta);
        fflush(stdout);
        return;
    }

    if (!g_alt) {
        fputs("\033[?1049h\033[?25l", stdout);
        g_alt = true;
    }

    const int W = term_width();
    const int L = 12;                                   /* label column */
    sb_t sb = { 0 };
    sb_add(&sb, "\033[H");
    sb_add(&sb, "\n  %s%sTM5 for Linux%s %s%s%s  %s%s%s%s\033[K\n", BOLD, CYAN, RST, DIM, TM5L_VERSION, RST,
           BOLD, WHT, s->title, RST);
    sb_add(&sb, "  %s", DIM);
    for (int i = 0; i < W - 4; i++)
        sb_add(&sb, "%s", g_utf8 ? "─" : "-");
    sb_add(&sb, "%s\033[K\n\033[K\n", RST);

    /* Status line */
    if (s->phase == PH_PREPARE) {
        sb_add(&sb, "  %s%-*s%s%s%s Preparing memory%s - allocating and locking %s (%.0f %%)\033[K\n", DIM, L, "Status", RST,
               YEL, g_utf8 ? "●" : "*", RST, mem, s->prep_pct);
    } else {
        const char *col = s->errors ? RED : CYAN;
        sb_add(&sb, "  %s%-*s%s%s%s%s Testing%s   cycle %s%d%s of %s%d%s  ·  step %s%d%s of %d", DIM, L, "Status", RST, BOLD,
               col, g_utf8 ? "●" : "*", RST, BOLD, s->cycle, RST, BOLD, s->cycles, RST, BOLD, s->step + 1, RST, s->nseq);
        if (s->cycles == 0)
            sb_add(&sb, "  %s(runs until stopped)%s", DIM, RST);
        sb_add(&sb, "\033[K\n");
        sb_add(&sb, "  %s%-*s%s%s#%d %s%s  %s\033[K\n", DIM, L, "Test", RST, BOLD, s->test_index, s->func, RST, s->summary);
        wrap(&sb, s->explain, W - L - 4, L + 2, 3);
    }
    sb_add(&sb, "\033[K\n");

    /* Numbers */
    sb_add(&sb, "  %s%-*s%s%s%-12s%s %sRemaining%s  ", DIM, L, "Elapsed", RST, BOLD, el, RST, DIM, RST);
    if (s->eta >= 0)
        sb_add(&sb, "%s~%s%s\033[K\n", BOLD, eta, RST);
    else
        sb_add(&sb, "%s%s%s\033[K\n", DIM, eta, RST);
    sb_add(&sb, "  %s%-*s%s%s%s%s on %d threads  ·  %s%s%s", DIM, L, "Memory", RST, BOLD, mem, RST, s->threads,
           s->locked ? GRN : YEL, s->locked ? "locked in RAM" : "NOT locked (may be swapped)", RST);
    if (s->huge_pct > 0)
        sb_add(&sb, "  ·  huge pages %.0f %%", s->huge_pct);
    sb_add(&sb, "\033[K\n");
    if (s->speed > 0)
        sb_add(&sb, "  %s%-*s%s%.1f GB/s\033[K\n", DIM, L, "Speed", RST, s->speed / 1e9);
    else
        sb_add(&sb, "  %s%-*s%s-\033[K\n", DIM, L, "Speed", RST);
    if (s->errors)
        sb_add(&sb, "  %s%-*s%s%s%s%llu  - your memory is NOT stable%s\033[K\n", DIM, L, "Errors", RST, BOLD, RED,
               (unsigned long long)s->errors, RST);
    else
        sb_add(&sb, "  %s%-*s%s%s%s0%s  %sno errors so far%s\033[K\n", DIM, L, "Errors", RST, BOLD, GRN, RST, DIM, RST);
    sb_add(&sb, "\033[K\n");

    /* Progress bars */
    int bw = W - L - 12;
    sb_add(&sb, "  %s%-*s%s", DIM, L, "Overall", RST);
    bar(&sb, s->phase == PH_PREPARE ? 0 : s->run_pct, bw, s->errors ? RED : GRN);
    sb_add(&sb, "\033[K\n  %s%-*s%s", DIM, L, s->phase == PH_PREPARE ? "Preparing" : "This test", RST);
    bar(&sb, s->phase == PH_PREPARE ? s->prep_pct : s->test_pct, bw, CYAN);
    sb_add(&sb, "\033[K\n\033[K\n");

    /* Sequence of this cycle, TM5 style: test numbers coloured by state */
    if (s->phase == PH_TESTING && s->seq && s->marks) {
        sb_add(&sb, "  %s%-*s%s", DIM, L, "Sequence", RST);
        int col = L + 2;
        for (int i = 0; i < s->nseq; i++) {
            char num[8];
            snprintf(num, sizeof num, "%d", s->seq[i]);
            int w = (int)strlen(num) + 1;
            if (col + w > W - 2) {
                sb_add(&sb, "\033[K\n%*s", L + 2, "");
                col = L + 2;
            }
            const char *c = DIM;
            switch (s->marks[i]) {
            case '+': c = GRN; break;
            case 'x': c = RED; break;
            case '>': c = g_color ? "\033[1;30;106m" : ""; break;
            }
            sb_add(&sb, "%s%s%s ", c, num, RST);
            col += w;
        }
        const char *sq = g_utf8 ? "■" : "#";
        sb_add(&sb, "\033[K\n  %*s%s%s%s passed   %s%s%s errors   %s%s%s running   %s%s waiting%s\033[K\n\033[K\n", L, "",
               GRN, sq, RST, RED, sq, RST, CYAN, sq, RST, DIM, sq, RST);
    }

    /* Recent events */
    sb_add(&sb, "  %sRecent events%s\033[K\n", DIM, RST);
    for (int i = 0; i < g_nev; i++)
        sb_add(&sb, "  %s%s%s  %s%s%s %.*s\033[K\n", DIM, g_ev[i].when, RST, lv_color(g_ev[i].level), lv_icon(g_ev[i].level),
               RST, W - 16, g_ev[i].text);
    sb_add(&sb, "\033[K\n  %sPress Ctrl+C to stop the test.%s\033[K\n\033[J", DIM, RST);

    fwrite(sb.buf, 1, sb.len, stdout);
    fflush(stdout);
    free(sb.buf);
}

void ui_end(void)
{
    if (g_alt) {
        fputs("\033[?25h\033[?1049l", stdout);
        fflush(stdout);
        g_alt = false;
    }
}

void ui_restore_terminal_async(void)
{
    if (g_alt) {
        static const char seq[] = "\033[?25h\033[?1049l";
        ssize_t r = write(STDOUT_FILENO, seq, sizeof seq - 1);
        (void)r;
    }
}
