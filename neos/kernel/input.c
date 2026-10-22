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

static void kbd_irq(void)
{
    while (inb(0x64) & 1) {
        u8 b = inb(0x60);
        if (b == 0x1D) irq_ctrl = 1; else if (b == 0x9D) irq_ctrl = 0;
        else if (b == 0x2B && irq_ctrl) agents_frozen = 1;      /* Ctrl+\ : emergency freeze of all agents */
        u8 n = (rh + 1) & 127;
        if (n != rt) { ring[rh] = b; rh = n; }
    }
}

void input_init(void)
{
    module_register("input", "1.0", "PS/2 keyboard (IRQ1) + serial console input");
    t_poll = tune_int("input", "poll_ms", "shell input polling interval (ms)", TF_LIVE, 2, 1, 100, NULL);
    while (inb(0x64) & 1) inb(0x60);
    irq_register(1, kbd_irq);
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
        return c;
    }
    return -1;
}

int key_poll(void)
{
    int k = from_serial();
    return k >= 0 ? k : from_ps2();
}
