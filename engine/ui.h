/* TM5 for Linux - terminal user interface (used when not in --json mode).
 * SPDX-License-Identifier: MIT
 */
#ifndef TM5L_UI_H
#define TM5L_UI_H

#include <stdbool.h>
#include <stdint.h>

enum { LV_INFO, LV_OK, LV_WARN, LV_ERR };

typedef enum { PH_PREPARE, PH_TESTING, PH_DONE } ui_phase_t;

typedef struct {
    char        title[300];
    ui_phase_t  phase;
    double      prep_pct;
    int         cycle, cycles;
    int         step, nseq;
    const int  *seq;
    const char *marks;          /* per step of this cycle: '.' waiting, '>' running, '+' ok, 'x' errors */
    int         test_index;
    char        func[32];
    char        summary[256];
    char        explain[1024];
    uint64_t    mem_bytes;
    int         threads;
    bool        locked;
    double      huge_pct;
    double      elapsed;
    double      eta;            /* < 0: unknown */
    double      speed;          /* bytes per second */
    double      run_pct, test_pct;
    uint64_t    errors;
} ui_state_t;

void ui_init(bool tty, bool color);
void ui_event(int level, const char *text);
void ui_draw(const ui_state_t *s);
void ui_end(void);
void ui_restore_terminal_async(void);
bool ui_color(void);
bool ui_utf8(void);

#endif
