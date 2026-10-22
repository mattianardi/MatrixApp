#pragma once
#include "kernel.h"

/* Keys produced by input.c: ASCII (Enter = '\n', Backspace = '\b'), Ctrl-A..Z = 1..26, or: */
enum { K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_PGUP, K_PGDN };
void input_init(void);
int  key_poll(void);
void key_inject(int c);             /* queue a key as if typed (GUI buttons) */
void mouse_state(int *x, int *y, int *btn, int *seq);
void mouse_bounds(int w, int h);
int  ui_cpu_pct(void);
void gui_init(void);
void gui_start(void);                 /* -1 if nothing is pending */

/* Structured command output: commands fill tables, the shell filters/renders/pins them. */
#define TB_COLS 8
#define TB_ROWS 40
#define TB_CELL 40
struct table {
    int ncols, nrows, more;
    char head[TB_COLS][16];
    char cell[TB_ROWS][TB_COLS][TB_CELL];
};
void tb_init(struct table *t, const char *c0, ...);          /* NULL-terminated header list */
int  tb_newrow(struct table *t);
void tb_set(struct table *t, int col, const char *fmt, ...);
void tb_widths(const struct table *t, int *w, int maxw);
void tb_format_row(const struct table *t, int row, const int *w, char *buf, size_t n);   /* row -1 = header */
void tb_render(const struct table *t);
void tb_json(const struct table *t);
int  tb_filter(struct table *t, int argc, char **argv, const char **err);

/* shell */
void shell_start(void);
int  shell_tokenize(char *s, char **argv, int max);
void shell_run_line(const char *line);
int  shell_eval_table(const char *pipeline, struct table *out, const char **err);   /* pure commands only */
const char *shell_prompt_hint(void);

/* ui.c: status bar + live dock in the same screen as the command stream */
void ui_init(void);
void ui_start(void);
int  ui_pin(const char *title, const char *pipeline, const char **err);
int  ui_unpin(int idx);
int  ui_pins(int idx, char *title, size_t tn, char *pipe, size_t pn);

/* command plumbing shared with extension modules (shell_ext.c) */
struct ctx { struct table *tb; const char *raw; int pure; };
