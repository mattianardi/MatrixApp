#include "shell.h"
#include "net.h"
#include "agent.h"
#include "varbus.h"

/*
 * The NEOS workspace. One screen: a desktop with the live terminal (the same command stream and dock as the
 * text console, shown in a window), a system monitor, an agents/approvals window, a taskbar and a mouse.
 * Nothing here is a separate application: every window is drawn from the same kernel state the shell shows,
 * and every button is just a shortcut (Approve = `agent approve`, AI Help types `aih ` into the shell).
 * The renderer is immediate-mode: each frame rebuilds the list of clickable regions it draws.
 */
#define SW 1024
#define SH 768
#define PANEL_H 24
#define TASK_H 34

static u32 *bb, *wall, *fb;                 /* back buffer, pre-rendered wallpaper, the BGA linear framebuffer */
static u8 font[256][16];
static int gui_on;
static struct tunable *t_enable, *t_ms;

static const u32 vga_pal[16] = { 0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                                 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF };
#define COL_BG    0x10151F
#define COL_WIN   0x1B2330
#define COL_TITLE 0x2D4F7C
#define COL_TITLE_OFF 0x2A3140
#define COL_ACC   0x4DA3FF
#define COL_TEXT  0xE6EDF7
#define COL_DIM   0x8E9BB0
#define COL_OK    0x3FB950
#define COL_WARN  0xE3B341
#define COL_BAD   0xF85149

/* ------------------------------------------------------------ BGA and font */
static void bga_write(u16 idx, u16 v) { outw(0x1CE, idx); outw(0x1CF, v); }
static u16 bga_read(u16 idx) { outw(0x1CE, idx); return inw(0x1CF); }

static void font_from_vga(void)
{
    u8 *plane = (u8 *)P2V(0xA0000);
    outb(0x3C4, 2); u8 s2 = inb(0x3C5); outb(0x3C5, 4);              /* write/read plane 2: the character generator */
    outb(0x3C4, 4); u8 s4 = inb(0x3C5); outb(0x3C5, 7);
    outb(0x3CE, 4); u8 g4 = inb(0x3CF); outb(0x3CF, 2);
    outb(0x3CE, 5); u8 g5 = inb(0x3CF); outb(0x3CF, 0);
    outb(0x3CE, 6); u8 g6 = inb(0x3CF); outb(0x3CF, 0);
    for (int c = 0; c < 256; c++) for (int r = 0; r < 16; r++) font[c][r] = plane[c * 32 + r];
    outb(0x3C4, 2); outb(0x3C5, s2); outb(0x3C4, 4); outb(0x3C5, s4);
    outb(0x3CE, 4); outb(0x3CF, g4); outb(0x3CE, 5); outb(0x3CF, g5); outb(0x3CE, 6); outb(0x3CF, g6);
}

/* ------------------------------------------------------------ drawing primitives (on bb) */
static void px(int x, int y, u32 c) { if ((unsigned)x < SW && (unsigned)y < SH) bb[y * SW + x] = c; }
static void rect(int x, int y, int w, int h, u32 c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    for (int j = 0; j < h; j++) { u32 *p = bb + (y + j) * SW + x; for (int i = 0; i < w; i++) p[i] = c; }
}
static void frame(int x, int y, int w, int h, u32 c) { rect(x, y, w, 1, c); rect(x, y + h - 1, w, 1, c); rect(x, y, 1, h, c); rect(x + w - 1, y, 1, h, c); }
static void glyph(int x, int y, int ch, u32 fg, int bgset, u32 bg, int sc)
{
    const u8 *g = font[ch & 255];
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) {
        int on = g[r] & (0x80 >> b);
        if (on || bgset) {
            u32 c = on ? fg : bg;
            if (sc == 1) px(x + b, y + r, c); else rect(x + b * sc, y + r * sc, sc, sc, c);
        }
    }
}
static int text(int x, int y, const char *s, u32 fg) { for (; *s; s++, x += 8) glyph(x, y, (u8)*s, fg, 0, 0, 1); return x; }
static void textc(int x, int y, int w, const char *s, u32 fg) { int l = (int)strlen(s); text(x + (w - l * 8) / 2, y, s, fg); }

/* ------------------------------------------------------------ windows and click regions */
enum { W_TERM, W_MON, W_AGENTS, NWIN };
struct win { int x, y, w, h; const char *title; int visible; };       /* w/h = client size */
static struct win wins[NWIN] = {
    { 20, 40, 640, 400, "Terminal - NEOS shell", 1 },
    { 690, 40, 310, 300, "System Monitor", 1 },
    { 430, 470, 570, 220, "Agents", 1 },
};
static int zorder[NWIN] = { W_MON, W_AGENTS, W_TERM };               /* back to front */
static int focus = W_TERM;

enum { R_BODY, R_TITLE, R_CLOSE, R_TASK, R_APPROVE, R_DENY, R_FREEZE, R_AIH };
struct region { int x, y, w, h, kind, arg; };
#define MAXREG 96
static struct region regs[MAXREG]; static int nregs;
static void add_region(int x, int y, int w, int h, int kind, int arg) { if (nregs < MAXREG) regs[nregs++] = (struct region){ x, y, w, h, kind, arg }; }

static int drag = -1, drag_dx, drag_dy;
static volatile int dirty = 1;

static void raise_win(int w)
{
    int i = 0; for (; i < NWIN && zorder[i] != w; i++) {}
    for (; i < NWIN - 1; i++) zorder[i] = zorder[i + 1];
    zorder[NWIN - 1] = w; focus = w;
}

static void button(int x, int y, int w, int h, const char *label, u32 col, int kind, int arg)
{
    rect(x, y, w, h, col); frame(x, y, w, h, 0x000000 | 0x3A4660);
    textc(x, y + (h - 16) / 2, w, label, COL_TEXT);
    add_region(x, y, w, h, kind, arg);
}

static void window_chrome(int id)
{
    struct win *w = &wins[id];
    int ox = w->x - 4, oy = w->y - 24, ow = w->w + 8, oh = w->h + 28;
    rect(ox + 5, oy + 5, ow, oh, 0x070A10);                              /* shadow */
    rect(ox, oy, ow, oh, COL_WIN); frame(ox, oy, ow, oh, focus == id ? COL_ACC : 0x3A4660);
    rect(ox + 1, oy + 1, ow - 2, 22, focus == id ? COL_TITLE : COL_TITLE_OFF);
    text(ox + 10, oy + 4, w->title, COL_TEXT);
    rect(ox + ow - 22, oy + 4, 14, 14, 0x7A2E2E); textc(ox + ow - 22 - 1, oy + 3, 16, "x", COL_TEXT);
    add_region(ox, oy, ow, oh, R_BODY, id);
    add_region(ox, oy, ow - 26, 24, R_TITLE, id);
    add_region(ox + ow - 24, oy + 2, 20, 20, R_CLOSE, id);
}

/* ------------------------------------------------------------ the terminal window: the text console, rendered */
static void draw_terminal(void)
{
    struct win *w = &wins[W_TERM];
    u16 *cells = console_cells(); int cr, cc; console_cursor(&cr, &cc);
    for (int r = 0; r < VGA_ROWS; r++) for (int c = 0; c < VGA_COLS; c++) {
        u16 v = cells[r * VGA_COLS + c]; u8 a = v >> 8;
        glyph(w->x + c * 8, w->y + r * 16, v & 255, vga_pal[a & 15], 1, vga_pal[(a >> 4) & 7], 1);
    }
    if (cr < VGA_ROWS && cc < VGA_COLS) rect(w->x + cc * 8, w->y + cr * 16 + 14, 8, 2, 0xFFFFFF);
}

/* ------------------------------------------------------------ system monitor */
#define HIST 60
static u8 cpu_hist[HIST]; static int hist_n;
static u64 last_sample; static int stats_epoch;

static void sample(void)
{
    u64 now = time_ns() / 1000000;
    if (now - last_sample < (u64)t_ms->val) return;
    last_sample = now; stats_epoch++;
    cpu_hist[hist_n++ % HIST] = (u8)ui_cpu_pct();
}

static void bar(int x, int y, int w, int pct, u32 col)
{
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    rect(x, y, w, 12, 0x0B0F16); rect(x, y, w * pct / 100, 12, col); frame(x, y, w, 12, 0x3A4660);
}

static void draw_monitor(void)
{
    struct win *w = &wins[W_MON]; char b[96];
    u64 up = time_ns() / 1000000000ull, mn, mx, avg, n; timer_jitter(&mn, &mx, &avg, &n);
    size_t total_mb = 494, free_mb = pmm_free_pages() / 256;
    int y = w->y + 8, x = w->x + 10;
    ksnprintf(b, sizeof b, "uptime   %02lu:%02lu:%02lu", up / 3600, up / 60 % 60, up % 60); text(x, y, b, COL_TEXT); y += 20;
    ksnprintf(b, sizeof b, "CPU      %d%%", ui_cpu_pct()); text(x, y, b, COL_TEXT); y += 20;
    bar(x, y, w->w - 20, ui_cpu_pct(), ui_cpu_pct() > 80 ? COL_BAD : COL_ACC); y += 20;
    int hx = x, hy = y, hh = 60;
    rect(hx, hy, HIST * 4, hh, 0x0B0F16); frame(hx, hy, HIST * 4, hh, 0x3A4660);
    for (int i = 0; i < HIST; i++) {
        int idx = (hist_n + i) % HIST, v = cpu_hist[idx];
        if (hist_n < HIST && idx >= hist_n) continue;
        rect(hx + i * 4, hy + hh - 1 - v * (hh - 2) / 100, 3, v * (hh - 2) / 100 + 1, COL_OK);
    }
    y += hh + 10;
    ksnprintf(b, sizeof b, "memory   %lu / %lu MiB free", (u64)free_mb, (u64)total_mb); text(x, y, b, COL_TEXT); y += 20;
    bar(x, y, w->w - 20, (int)((total_mb - free_mb) * 100 / total_mb), COL_WARN); y += 22;
    ksnprintf(b, sizeof b, "timer    %u Hz, jitter <= %lu us", timer_hz(), mx / 1000); text(x, y, b, COL_TEXT); y += 20;
    char ip[24] = "-"; bus_get("NEOS.net.ip", NEOS_VIEW_LINUX, ip, sizeof ip);
    ksnprintf(b, sizeof b, "network  %s", ip); text(x, y, b, COL_TEXT);
}

/* ------------------------------------------------------------ agents */
static void draw_agents(void)
{
    struct win *w = &wins[W_AGENTS]; char b[128];
    int y = w->y + 6, x = w->x + 10, na = 0;
    text(x, y, "AGENT       KIND    CALLS  DENIED  STATE", COL_DIM); y += 18;
    for (int i = 0; i < AG_MAX && y < w->y + 92; i++) {
        struct agent *a = agent_at(i); if (!a) continue; na++;
        ksnprintf(b, sizeof b, "%-11s %-7s %5lu  %6lu  %s", a->name, a->kind == AK_NATIVE ? "native" : "remote", a->calls, a->denied, a->state == AS_ACTIVE ? "active" : "suspended");
        text(x, y, b, COL_TEXT); y += 16;
    }
    y = w->y + 100;
    int pend = approval_count_pending();
    ksnprintf(b, sizeof b, "Pending approvals: %d", pend); text(x, y, b, pend ? COL_WARN : COL_DIM); y += 20;
    for (int i = 0; i < 2 && i < pend; i++) {
        struct approval ap; if (approval_get(i, &ap)) break;
        struct agent *a = agent_at(ap.agent);
        ksnprintf(b, sizeof b, "%s: %s %s", a ? a->name : "?", ap.tool, ap.args); b[44] = 0;
        text(x, y + 2, b, COL_TEXT);
        button(w->x + w->w - 150, y, 66, 20, "Approve", 0x1F6B35, R_APPROVE, ap.id);
        button(w->x + w->w - 78, y, 60, 20, "Deny", 0x7A2E2E, R_DENY, ap.id);
        y += 26;
    }
    int frozen = agent_is_frozen();
    button(w->x + 10, w->y + w->h - 32, 170, 24, frozen ? "Thaw agents" : "Freeze all agents", frozen ? 0x1F6B35 : 0x7A2E2E, R_FREEZE, 0);
    text(w->x + 196, w->y + w->h - 28, frozen ? "AGENTS FROZEN" : "agents running under policy", frozen ? COL_BAD : COL_DIM);
}

/* ------------------------------------------------------------ desktop, panel, taskbar, cursor */
static void render_wallpaper(void)
{
    for (int y = 0; y < SH; y++) {
        int t = y * 255 / SH; u32 c = ((16 + t / 12) << 16) | ((24 + t / 8) << 8) | (48 + t / 3);
        for (int x = 0; x < SW; x++) wall[y * SW + x] = c;
    }
    u32 *save = bb; bb = wall;
    for (int ch = 0; ch < 4; ch++) glyph(SW - 330 + ch * 64, SH - 200, "NEOS"[ch], 0x24456E, 0, 0, 8);
    text(SW - 372, SH - 66, "Real-Time Operating System - by Mattia Nardi", 0x3A5F8F);
    bb = save;
}

static void draw_panel(void)
{
    char b[128];
    rect(0, 0, SW, PANEL_H, 0x0B1018); rect(0, PANEL_H - 1, SW, 1, 0x2C3B52);
    text(10, 4, NEOS_NAME " " NEOS_VERSION, COL_ACC);
    u64 up = time_ns() / 1000000000ull; ksnprintf(b, sizeof b, "up %02lu:%02lu:%02lu   cpu %d%%   mem %luM free", up / 3600, up / 60 % 60, up % 60, ui_cpu_pct(), (u64)pmm_free_pages() / 256);
    text(SW / 2 - 150, 4, b, COL_TEXT);
    int pend = approval_count_pending();
    if (agent_is_frozen()) { rect(SW - 190, 2, 180, 20, COL_BAD); textc(SW - 190, 4, 180, "AGENTS FROZEN", COL_TEXT); }
    else if (pend) { ksnprintf(b, sizeof b, "APPROVE: %d pending", pend); rect(SW - 190, 2, 180, 20, 0x8A6A00); textc(SW - 190, 4, 180, b, COL_TEXT); }
    else { int n = 0; for (int i = 0; i < AG_MAX; i++) n += agent_at(i) != NULL; ksnprintf(b, sizeof b, "agents: %d", n); text(SW - 190, 4, b, COL_DIM); }
}

static void draw_taskbar(void)
{
    rect(0, SH - TASK_H, SW, TASK_H, 0x0B1018); rect(0, SH - TASK_H, SW, 1, 0x2C3B52);
    static const char *names[NWIN] = { "Terminal", "Monitor", "Agents" };
    int x = 10;
    for (int i = 0; i < NWIN; i++, x += 112) button(x, SH - TASK_H + 5, 104, 24, names[i], wins[i].visible ? (focus == i ? COL_TITLE : 0x25354D) : 0x161D29, R_TASK, i);
    button(x + 20, SH - TASK_H + 5, 120, 24, "AI Help (aih)", 0x5B3FA0, R_AIH, 0);
    text(SW - 330, SH - TASK_H + 9, "click, drag titles, type in the Terminal", COL_DIM);
}

static const char *const arrow[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
    "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ", "      XX    " };
static void draw_cursor(int mx, int my)
{
    for (int r = 0; r < 17; r++) for (int c = 0; c < 12; c++) {
        char ch = arrow[r][c];
        if (ch == 'X') px(mx + c, my + r, 0x000000); else if (ch == '.') px(mx + c, my + r, 0xFFFFFF);
    }
}

/* ------------------------------------------------------------ frame + input */
static void frame_draw(int mx, int my)
{
    memcpy(bb, wall, (size_t)SW * SH * 4);
    nregs = 0;
    draw_panel();
    for (int i = 0; i < NWIN; i++) {
        int id = zorder[i]; if (!wins[id].visible) continue;
        window_chrome(id);
        if (id == W_TERM) draw_terminal(); else if (id == W_MON) draw_monitor(); else draw_agents();
    }
    draw_taskbar();
    draw_cursor(mx, my);
}

static void flush(void)
{
    u64 *s = (u64 *)bb, *d = (u64 *)fb; size_t n = (size_t)SW * SH / 2;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void click(int mx, int my)
{
    for (int i = nregs - 1; i >= 0; i--) {
        struct region *r = &regs[i];
        if (mx < r->x || my < r->y || mx >= r->x + r->w || my >= r->y + r->h) continue;
        switch (r->kind) {
        case R_BODY: raise_win(r->arg); break;
        case R_TITLE: raise_win(r->arg); drag = r->arg; drag_dx = mx - wins[r->arg].x; drag_dy = my - wins[r->arg].y; break;
        case R_CLOSE: wins[r->arg].visible = 0; if (focus == r->arg) focus = W_TERM; break;
        case R_TASK: wins[r->arg].visible = !wins[r->arg].visible || focus != r->arg; if (wins[r->arg].visible) raise_win(r->arg); break;
        case R_APPROVE: approval_decide(r->arg, 1); break;
        case R_DENY: approval_decide(r->arg, 0); break;
        case R_FREEZE: if (agent_is_frozen()) agent_thaw(); else agent_freeze(); break;
        case R_AIH: wins[W_TERM].visible = 1; raise_win(W_TERM); key_inject('a'); key_inject('i'); key_inject('h'); key_inject(' '); break;
        }
        dirty = 1;
        return;
    }
}

static void gui_thread(void *arg)
{
    (void)arg;
    int lx = -1, ly = -1, lb = 0, lseq = -1, lstats = -1; u32 lsig = 0;
    for (;;) {
        thread_sleep_ms((u64)t_ms->val / 10 > 20 ? (u64)t_ms->val / 10 : 20);
        if (!t_enable->val) continue;
        int mx, my, mb, seq; mouse_state(&mx, &my, &mb, &seq);
        sample();
        if (mb & 1) {
            if (!(lb & 1)) click(mx, my);
            else if (drag >= 0) {
                struct win *w = &wins[drag];
                w->x = mx - drag_dx; w->y = my - drag_dy;
                if (w->y < 28) w->y = 28;
                if (w->x < 4) w->x = 4;
                if (w->y > SH - TASK_H - 10) w->y = SH - TASK_H - 10;
                if (w->x > SW - 40) w->x = SW - 40;
            }
        } else drag = -1;
        /* cheap change signature: cells, cursor, pointer, stats epoch, approvals */
        u16 *cells = console_cells(); u32 sig = 2166136261u;
        for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) sig = (sig ^ cells[i]) * 16777619u;
        int cr, cc; console_cursor(&cr, &cc); sig = (sig ^ (u32)(cr * 100 + cc)) * 16777619u;
        sig = (sig ^ (u32)(approval_count_pending() * 7 + agent_is_frozen())) * 16777619u;
        if (!dirty && mx == lx && my == ly && mb == lb && seq == lseq && stats_epoch == lstats && sig == lsig) continue;
        dirty = 0; lx = mx; ly = my; lb = mb; lseq = seq; lstats = stats_epoch; lsig = sig;
        frame_draw(mx, my);
        flush();
    }
}

void gui_init(void)
{
    t_enable = tune_bool("gui", "enable", "draw the graphical workspace", TF_LIVE, 1, NULL);
    t_ms = tune_int("gui", "stats_ms", "monitor refresh period (ms); the screen itself redraws only when something changes", TF_LIVE, 500, 100, 5000, NULL);
    struct pci_dev d;
    if (pci_find(0x1234, 0x1111, &d) || bga_read(0) < 0xB0C0) return;           /* no Bochs VBE display adapter */
    font_from_vga();
    u32 bar = pci_read(d.bus, d.dev, d.fn, 0x10) & ~0xFu;
    pci_write(d.bus, d.dev, d.fn, 4, pci_read(d.bus, d.dev, d.fn, 4) | 0x7);
    if (!bar) return;
    u64 pb = pmm_alloc(SW * SH * 4 / 4096), pw = pmm_alloc(SW * SH * 4 / 4096), pc = pmm_alloc(1);
    if (!pb || !pw || !pc) return;
    bb = P2V(pb); wall = P2V(pw); fb = P2V(bar);
    console_use_cells(P2V(pc));                                                  /* take the text cells before the legacy window disappears */
    bga_write(4, 0); bga_write(1, SW); bga_write(2, SH); bga_write(3, 32); bga_write(4, 0x41);
    render_wallpaper();
    mouse_bounds(SW, SH);
    module_register("gui", "1.0", "graphical workspace on the Bochs VBE framebuffer: windows, mouse, taskbar");
    gui_on = 1;
}

void gui_start(void) { if (gui_on) thread_create("gui", 6, gui_thread, NULL); }
