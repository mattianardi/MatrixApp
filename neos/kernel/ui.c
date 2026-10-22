#include "shell.h"
#include "varbus.h"

/*
 * One screen, one surface: row 0 is a live status bar, below it an optional dock
 * of pinned live views (any pure table command: `ps | where prio<4 | pin`), then
 * the normal scrolling command stream. Nothing to switch to - pin what you
 * want to watch and keep typing.
 */
#define MAX_PINS 4
static struct { int used; char title[24]; char pipe[96]; } pins[MAX_PINS];
static struct table tb;                 /* only touched by the UI thread */
static struct tunable *t_ms, *t_dock, *t_status, *t_rows;
static int dock_h;
static u64 last_busy, last_total, last_idle;
static int cpu_pct;

void ui_init(void)
{
    module_register("ui", "1.0", "unified screen: status bar + live pinned views + command stream");
    t_ms     = tune_int ("ui", "refresh_ms", "status bar / dock refresh period", TF_LIVE, 500, 50, 10000, NULL);
    t_dock   = tune_int ("ui", "dock_max",   "max dock height in rows", TF_LIVE, 10, 0, 16, NULL);
    t_rows   = tune_int ("ui", "pin_rows",   "max table rows shown per pinned view", TF_LIVE, 4, 1, 12, NULL);
    t_status = tune_bool("ui", "status_bar", "show the live status bar", TF_LIVE, 1, NULL);
}

static void layout(void)
{
    int h = 0;
    for (int i = 0; i < MAX_PINS; i++) if (pins[i].used) h += 1 + 1 + (int)t_rows->val;
    if (h > t_dock->val) h = (int)t_dock->val;
    int top = (t_status->val ? 1 : 0) + h;
    if (h != dock_h || top != (t_status->val ? 1 : 0) + dock_h) {
        for (int r = 0; r < top; r++) vga_write(r, 0, "", 0x07, VGA_COLS);
        dock_h = h;
    }
    console_set_top(top);
}

static void status_bar(void)
{
    u64 up = time_ns() / 1000000000ull, mn, mx, avg, n;
    timer_jitter(&mn, &mx, &avg, &n);
    u64 total = sched_runticks_total(), idle = 0;
    struct thread_info ti;
    if (!thread_info(0, &ti)) idle = ti.runticks;
    if (total > last_total) { cpu_pct = (int)(100 - (idle - last_idle) * 100 / (total - last_total)); last_total = total; last_idle = idle; }
    char ip[24] = "-", line[VGA_COLS + 1];
    bus_get("NEOS.net.ip", NEOS_VIEW_LINUX, ip, sizeof ip);
    ksnprintf(line, sizeof line, " %s %s | up %02lu:%02lu:%02lu | cpu %d%% | mem %luM free | %uHz jitter<=%luus | ip %s",
              NEOS_NAME, NEOS_VERSION, up / 3600, up / 60 % 60, up % 60, cpu_pct,
              (u64)pmm_free_pages() / 256, timer_hz(), mx / 1000, ip);
    vga_write(0, 0, line, 0x30, VGA_COLS);
}

static void draw_dock(void)
{
    int row = t_status->val ? 1 : 0, w[TB_COLS];
    for (int i = 0; i < MAX_PINS && row < (t_status->val ? 1 : 0) + dock_h; i++) {
        if (!pins[i].used) continue;
        char line[VGA_COLS + 1]; const char *err = "";
        ksnprintf(line, sizeof line, "-- %s  [%s] ", pins[i].title, pins[i].pipe);
        vga_write(row++, 0, line, 0x1B, VGA_COLS);
        int ok = shell_eval_table(pins[i].pipe, &tb, &err) == 0;
        if (!ok) { vga_write(row++, 0, err, 0x0C, VGA_COLS); continue; }
        tb_widths(&tb, w, 24);
        tb_format_row(&tb, -1, w, line, sizeof line);
        vga_write(row++, 0, line, 0x0B, VGA_COLS);
        for (int r = 0; r < t_rows->val && row < (t_status->val ? 1 : 0) + dock_h; r++) {
            if (r < tb.nrows) tb_format_row(&tb, r, w, line, sizeof line); else line[0] = 0;
            vga_write(row++, 0, line, 0x07, VGA_COLS);
        }
    }
}

static void ui_thread(void *arg)
{
    (void)arg;
    for (;;) {
        layout();
        if (t_status->val) status_bar();
        draw_dock();
        thread_sleep_ms((u64)t_ms->val);
    }
}

void ui_start(void) { thread_create("ui", 5, ui_thread, NULL); }

int ui_pin(const char *title, const char *pipeline, const char **err)
{
    static struct table pr;     /* avoid a large stack object; only the shell thread pins */
    if (shell_eval_table(pipeline, &pr, err)) return -1;
    for (int i = 0; i < MAX_PINS; i++) if (!pins[i].used) {
        strlcpy(pins[i].title, title, sizeof pins[i].title);
        strlcpy(pins[i].pipe, pipeline, sizeof pins[i].pipe);
        pins[i].used = 1;
        return i;
    }
    *err = "all dock slots are in use (unpin one)";
    return -1;
}

int ui_unpin(int idx)
{
    if (idx < 0 || idx >= MAX_PINS || !pins[idx].used) return -1;
    pins[idx].used = 0;
    return 0;
}

int ui_pins(int idx, char *title, size_t tn, char *pipe, size_t pn)
{
    if (idx < 0 || idx >= MAX_PINS || !pins[idx].used) return -1;
    strlcpy(title, pins[idx].title, tn); strlcpy(pipe, pins[idx].pipe, pn);
    return 0;
}
