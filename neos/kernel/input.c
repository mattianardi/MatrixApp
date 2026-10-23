#include "shell.h"

#define COM1 0x3F8
static u8 ring[128];
static volatile u8 rh, rt;
static int shift, ctrl, caps, e0, ser_esc, ser_n, skip_lf;
static char ser_buf[4];
static struct tunable *t_poll;

static const char norm[] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char shft[] = "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

volatile int agents_frozen;             /* set from the IRQ: works even if every thread is busy */
static int irq_ctrl;

/* ---- PS/2 mouse (aux port, IRQ12). The controller multiplexes keyboard and mouse bytes; status bit 5 says which. ---- */
static volatile int m_x = 512, m_y = 384, m_btn, m_seq, m_w = 1024, m_h = 768;
static u8 m_pkt[3]; static int m_n;
static int kinject[16]; static volatile u8 kih, kit;

void evdev_key(int ch);
void evdev_mouse_rel(int dx, int dy, int btn);
void evdev_mouse_abs(int x, int y, int max, int btn);

static void mouse_feed(u8 b)
{
    if (m_n == 0 && !(b & 8)) return;                              /* out of sync: byte 0 always has bit 3 set */
    m_pkt[m_n++] = b;
    if (m_n < 3) return;
    m_n = 0;
    int dx = m_pkt[1] - ((m_pkt[0] << 4) & 0x100), dy = m_pkt[2] - ((m_pkt[0] << 3) & 0x100);
    int x = m_x + dx, y = m_y - dy;
    m_x = x < 0 ? 0 : x >= m_w ? m_w - 1 : x;
    m_y = y < 0 ? 0 : y >= m_h ? m_h - 1 : y;
    m_btn = m_pkt[0] & 7;
    m_seq++;
    evdev_mouse_rel(dx, -dy, m_btn);
}

static void ps2_irq(void)
{
    u8 st;
    while ((st = inb(0x64)) & 1) {
        u8 b = inb(0x60);
        if (st & 0x20) { mouse_feed(b); continue; }
        if (b == 0x1D) irq_ctrl = 1; else if (b == 0x9D) irq_ctrl = 0;
        else if (b == 0x2B && irq_ctrl) agents_frozen = 1;      /* Ctrl+\ : emergency freeze of all agents */
        u8 n = (rh + 1) & 127;
        if (n != rt) { ring[rh] = b; rh = n; }
    }
}

static int ps2_wait(int write) { for (int i = 0; i < 100000; i++) { u8 s = inb(0x64); if (write ? !(s & 2) : (s & 1)) return 0; } return -1; }
static int mouse_cmd(u8 c)
{
    if (ps2_wait(1)) return -1;
    outb(0x64, 0xD4);
    if (ps2_wait(1)) return -1;
    outb(0x60, c);
    if (ps2_wait(0)) return -1;
    return inb(0x60) == 0xFA ? 0 : -1;
}

static int mouse_init(void)
{
    if (ps2_wait(1)) return -1;
    outb(0x64, 0xA8);                                              /* enable the aux port */
    if (ps2_wait(1)) return -1;
    outb(0x64, 0x20);
    if (ps2_wait(0)) return -1;
    u8 cfg = inb(0x60);
    cfg = (cfg | 2) & ~0x20;                                       /* aux IRQ on, aux clock on */
    if (ps2_wait(1)) return -1;
    outb(0x64, 0x60);
    if (ps2_wait(1)) return -1;
    outb(0x60, cfg);
    if (mouse_cmd(0xF6) || mouse_cmd(0xF4)) return -1;             /* defaults, enable streaming */
    return 0;
}

/* ---- sinks for USB HID drivers ---- */
static int ukeys[32]; static volatile u8 ukh, ukt;
void input_push_key(int ch)
{
    if (ch == 0x1C) { agents_frozen = 1; return; }                    /* Ctrl+\ */
    u8 n = (ukh + 1) & 31; if (n != ukt) { ukeys[ukh] = ch; ukh = n; }
}
void input_mouse_rel(int dx, int dy, int btn)
{
    int x = m_x + dx, y = m_y + dy;
    m_x = x < 0 ? 0 : x >= m_w ? m_w - 1 : x;
    m_y = y < 0 ? 0 : y >= m_h ? m_h - 1 : y;
    m_btn = btn & 7; m_seq++;
    evdev_mouse_rel(dx, dy, btn & 7);
}
void input_mouse_abs(int x, int y, int max, int btn)
{
    m_x = x * (m_w - 1) / max; m_y = y * (m_h - 1) / max; m_btn = btn & 7; m_seq++;
    evdev_mouse_abs(x, y, max, btn & 7);
}

void mouse_state(int *x, int *y, int *btn, int *seq) { *x = m_x; *y = m_y; *btn = m_btn; *seq = m_seq; }
void mouse_bounds(int w, int h) { m_w = w; m_h = h; if (m_x >= w) m_x = w - 1; if (m_y >= h) m_y = h - 1; }
void key_inject(int c) { u8 n = (kih + 1) & 15; if (n != kit) { kinject[kih] = c; kih = n; } }

void input_init(void)
{
    module_register("input", "1.0", "PS/2 keyboard (IRQ1) + serial console input");
    t_poll = tune_int("input", "poll_ms", "shell input polling interval (ms)", TF_LIVE, 2, 1, 100, NULL);
    while (inb(0x64) & 1) inb(0x60);
    struct tunable *t_ps2 = tune_bool("input", "ps2", "use the PS/2 keyboard and mouse (off: USB devices only)", TF_BOOT, 1, NULL);
    if (!t_ps2->val) return;
    irq_register(1, ps2_irq);
    if (!mouse_init()) { module_register("mouse", "1.0", "PS/2 mouse (IRQ12)"); irq_register(12, ps2_irq); }
}

static int from_serial(void)
{
    while (inb(COM1 + 5) & 1) {
        u8 c = inb(COM1);
        if (ser_esc == 0) {
            if (c == 0x1B) { ser_esc = 1; ser_n = 0; continue; }
            if (c == '\r') { skip_lf = 1; return '\n'; }
            if (c == '\n') { if (skip_lf) { skip_lf = 0; continue; } return '\n'; }
            skip_lf = 0;
            if (c == 0x7F) return '\b';
            return c;
        }
        if (ser_esc == 1) { if (c == '[' || c == 'O') { ser_esc = 2; ser_n = 0; continue; } ser_esc = 0; return 0x1B; }
        if (is_digit(c) || c == ';') { if (ser_n < 3) ser_buf[ser_n++] = c; continue; }
        ser_esc = 0;
        switch (c) {
        case 'A': return K_UP;   case 'B': return K_DOWN;
        case 'C': return K_RIGHT; case 'D': return K_LEFT;
        case 'H': return K_HOME; case 'F': return K_END;
        case '~': switch (ser_buf[0]) { case '1': case '7': return K_HOME; case '3': return K_DEL; case '4': case '8': return K_END; case '5': return K_PGUP; case '6': return K_PGDN; }
        }
    }
    return -1;
}

static int from_ps2(void)
{
    if (ukt != ukh) { int c = ukeys[ukt]; ukt = (ukt + 1) & 31; evdev_key(c); return c; }
    while (rt != rh) {
        u8 sc = ring[rt]; rt = (rt + 1) & 127;
        if (sc == 0xE0) { e0 = 1; continue; }
        int rel = sc & 0x80; sc &= 0x7F;
        if (sc == 0x2A || sc == 0x36) { shift = !rel; e0 = 0; continue; }
        if (sc == 0x1D) { ctrl = !rel; e0 = 0; continue; }
        if (sc == 0x3A && !rel) { caps = !caps; continue; }
        if (rel) { e0 = 0; continue; }
        if (e0) {
            e0 = 0;
            switch (sc) {
            case 0x48: return K_UP;   case 0x50: return K_DOWN;
            case 0x4B: return K_LEFT; case 0x4D: return K_RIGHT;
            case 0x47: return K_HOME; case 0x4F: return K_END;
            case 0x53: return K_DEL;  case 0x49: return K_PGUP; case 0x51: return K_PGDN;
            case 0x1C: return '\n';
            }
            continue;
        }
        if (sc >= sizeof norm - 1) continue;
        int c = (shift ? shft : norm)[sc];
        if (!c) continue;
        if (caps && is_alpha(c)) c = shift ? to_lower(c) : to_upper(c);
        if (ctrl && is_alpha(c)) c = to_lower(c) - 'a' + 1;
        evdev_key(c);
        return c;
    }
    return -1;
}

volatile int key_grab;                  /* set by the workspace while a window other than the terminal owns the (PS/2) keyboard; the serial console always stays with the shell */

int key_poll(void)
{
    if (key_grab) return from_serial();
    if (kit != kih) { int c = kinject[kit]; kit = (kit + 1) & 15; return c; }
    int k = from_serial();
    return k >= 0 ? k : from_ps2();
}

int key_poll_raw(void) { return from_ps2(); }
