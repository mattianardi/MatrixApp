#include "serial.h"
#include "vfs.h"
#include "proc.h"
#include "vm.h"

#define RXSZ 8192
#define TXSZ 8192
struct serdev {
    int used; char name[16];
    u8 *rx, *tx; volatile u32 rh, rt, th, tt;
    u8 term[44];                                                    /* struct termios2 */
    u32 modem; volatile int dirty;                                  /* TIOCM_DTR / TIOCM_RTS requested by the program */
    u32 gen; void *priv;
};
static struct serdev sers[SER_MAX];

/* c_cflag baud codes -> bits per second */
static const u32 baud_tab[] = { 0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400 };
static const u32 baud_hi[] = { 57600, 115200, 230400, 460800, 500000, 576000, 921600, 1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000 };
static u32 baud_of(const u8 *t)
{
    u32 cf = *(u32 *)(t + 8), c = cf & 0x100F;
    if (cf & 0x1000 /* BOTHER */ && !(c & 0xF)) return *(u32 *)(t + 40);
    if (c <= 15) return baud_tab[c];
    if (c >= 0x1001 && c <= 0x100F) return baud_hi[c - 0x1001];
    return 9600;
}
static u32 code_of(u32 b)
{
    for (u32 i = 1; i < 16; i++) if (baud_tab[i] == b) return i;
    for (u32 i = 0; i < 15; i++) if (baud_hi[i] == b) return 0x1001 + i;
    return 0x1000;                                                  /* BOTHER */
}

int serial_register(const char *name, void *priv)
{
    for (int i = 0; i < SER_MAX; i++) if (!sers[i].used) {
        struct serdev *s = &sers[i];
        memset(s, 0, sizeof *s);
        s->rx = kmalloc(RXSZ); s->tx = kmalloc(TXSZ);
        if (!s->rx || !s->tx) { if (s->rx) kfree(s->rx); if (s->tx) kfree(s->tx); return -1; }
        strlcpy(s->name, name, sizeof s->name); s->priv = priv; s->used = 1;
        u32 *t = (u32 *)s->term;
        t[0] = 0; t[1] = 0; t[2] = 0x8BD /* B9600|CS8|CREAD|CLOCAL */; t[3] = 0; s->term[17 + 6] = 1 /* VMIN */; s->term[17 + 5] = 0;
        t[9] = t[10] = 9600;
        char path[32]; ksnprintf(path, sizeof path, "/dev/%s", name);
        struct inode *ino;
        if (!vfs_create(path, S_IFCHR | 0666, &ino)) ino->dev = DEV_SERIAL0 + i;
        s->dirty = 1;
        return i;
    }
    return -1;
}
int serial_count(void) { int n = 0; for (int i = 0; i < SER_MAX; i++) n += sers[i].used; return n; }
const char *serial_name(int id) { return id >= 0 && id < SER_MAX && sers[id].used ? sers[id].name : NULL; }
void *serial_priv(int id) { return sers[id].priv; }
u32 serial_in_gen(int id) { return sers[id].gen; }

void serial_rx(int id, const u8 *d, int n)
{
    struct serdev *s = &sers[id];
    for (int i = 0; i < n; i++) {
        if (s->rh - s->rt >= RXSZ) break;                           /* overrun: drop, like hardware */
        s->rx[s->rh % RXSZ] = d[i]; s->rh++;
    }
    if (n) { s->gen++; wake_on(s, 64); }
}
int serial_tx_pull(int id, u8 *buf, int max)
{
    struct serdev *s = &sers[id]; int n = 0;
    while (n < max && s->th != s->tt) { buf[n++] = s->tx[s->tt % TXSZ]; s->tt++; }
    if (n) wake_on(&s->tx, 64);
    return n;
}
int serial_dirty(int id) { int d = sers[id].dirty; sers[id].dirty = 0; return d; }
void serial_params(int id, u32 *baud, int *bits, int *parity, int *stop2, int *dtr, int *rts)
{
    struct serdev *s = &sers[id]; u32 cf = *(u32 *)(s->term + 8);
    *baud = baud_of(s->term); *bits = 5 + (int)((cf >> 4) & 3);
    *parity = !(cf & 0x100) ? 0 : (cf & 0x200) ? 1 : 2;           /* 0 none, 1 odd, 2 even */
    *stop2 = (cf & 0x40) != 0; *dtr = (s->modem & 2) != 0; *rts = (s->modem & 4) != 0;
}

long serial_read(int id, void *buf, size_t n, int nonblock)
{
    struct serdev *s = &sers[id]; u8 *o = buf;
    u8 vmin = s->term[17 + 6], vtime = s->term[17 + 5];
    u64 deadline = ~0ull;
    if (!n) return 0;
    for (;;) {
        if (s->rh != s->rt) break;
        if (nonblock) return -E_AGAIN;
        if (!vmin && !vtime) return 0;
        if (!vmin && vtime) { if (deadline == ~0ull) deadline = ticks + ms_to_ticks((u64)vtime * 100); else if (ticks >= deadline) return 0; }
        u64 fl = irq_save();
        if (s->rh == s->rt) sleep_on_locked(s, deadline == ~0ull ? ~0ull : deadline);
        irq_restore(fl);
        if (deadline != ~0ull && ticks >= deadline && s->rh == s->rt) return 0;
    }
    size_t c = 0;
    while (c < n && s->rh != s->rt) { o[c++] = s->rx[s->rt % RXSZ]; s->rt++; }
    return (long)c;
}
long serial_write(int id, const void *buf, size_t n, int nonblock)
{
    struct serdev *s = &sers[id]; const u8 *in = buf; size_t done = 0;
    while (done < n) {
        while (s->th - s->tt >= TXSZ) {
            if (nonblock) return done ? (long)done : -E_AGAIN;
            u64 fl = irq_save();
            if (s->th - s->tt >= TXSZ) sleep_on_locked(&s->tx, ticks + ms_to_ticks(50));
            irq_restore(fl);
        }
        while (done < n && s->th - s->tt < TXSZ) { s->tx[s->th % TXSZ] = in[done++]; s->th++; }
    }
    return (long)done;
}
int serial_poll(int id, int events)
{
    struct serdev *s = &sers[id]; int r = 0;
    if ((events & 1) && s->rh != s->rt) r |= 1;
    if ((events & 4) && s->th - s->tt < TXSZ) r |= 4;
    return r;
}

long serial_ioctl(int id, u64 req, u64 arg, void *proc)
{
    struct proc *p = proc; struct serdev *s = &sers[id];
    switch (req) {
    case 0x5401: return copy_to_user(&p->as, arg, s->term, 36) ? -E_FAULT : 0;                 /* TCGETS */
    case 0x802C542A: return copy_to_user(&p->as, arg, s->term, 44) ? -E_FAULT : 0;             /* TCGETS2 */
    case 0x5402: case 0x5403: case 0x5404: case 0x402C542B: case 0x402C542C: case 0x402C542D: {
        int two = req >= 0x402C542B; u8 t[44]; memcpy(t, s->term, 44);
        if (copy_from_user(&p->as, t, arg, two ? 44 : 36)) return -E_FAULT;
        if (!two) { u32 b = baud_of(t); *(u32 *)(t + 36) = *(u32 *)(t + 40) = b; }               /* TCSETS: speed comes from the CBAUD bits */
        else if (!(*(u32 *)(t + 8) & 0x1000)) { u32 b = baud_of(t); *(u32 *)(t + 36) = *(u32 *)(t + 40) = b; }
        memcpy(s->term, t, 44); s->dirty = 1;
        return 0;
    }
    case 0x5415: { u32 m = s->modem | 0x20 | 0x40 | 0x100; return copy_to_user(&p->as, arg, &m, 4) ? -E_FAULT : 0; }     /* TIOCMGET: CTS, CD, DSR always asserted */
    case 0x5418: case 0x5416: case 0x5417: {                                                    /* TIOCMSET / BIS / BIC */
        u32 m; if (copy_from_user(&p->as, &m, arg, 4)) return -E_FAULT; m &= 6;
        s->modem = req == 0x5418 ? m : req == 0x5416 ? s->modem | m : s->modem & ~m; s->dirty = 1; return 0;
    }
    case 0x541B: { int n = (int)(s->rh - s->rt); return copy_to_user(&p->as, arg, &n, 4) ? -E_FAULT : 0; }     /* FIONREAD */
    case 0x5411: { int n = (int)(s->th - s->tt); return copy_to_user(&p->as, arg, &n, 4) ? -E_FAULT : 0; }     /* TIOCOUTQ */
    case 0x540B: { if (arg == 0 || arg == 2) s->rt = s->rh; if (arg == 1 || arg == 2) s->tt = s->th; return 0; }   /* TCFLSH */
    case 0x5409: { for (int i = 0; i < 5000 && s->th != s->tt; i++) thread_sleep_ms(1); return 0; }                 /* TCSBRK: drain */
    case 0x540C: case 0x540D: case 0x540E: case 0x5422: case 0x541F: case 0x5428: return 0;                       /* TIOCEXCL, TIOCNXCL, TIOCSCTTY, TIOCSSERIAL, ... */
    case 0x541E: { u8 z[72]; memset(z, 0, sizeof z); return copy_to_user(&p->as, arg, z, sizeof z) ? -E_FAULT : 0; }  /* TIOCGSERIAL */
    case 0x5413: { u16 ws[4] = { 0, 0, 0, 0 }; return copy_to_user(&p->as, arg, ws, 8) ? -E_FAULT : 0; }
    }
    return -E_NOTTY;
}

/* ---------------------------------------------------------------- 16550 UARTs on COM3 / COM4 (0x3E8, 0x2E8), polled.
 * COM1 is the console and COM2 carries the agent channel (agent_chan.c), so they are not offered as serial ports. */
static const u16 uart_base[2] = { 0x3E8, 0x2E8 };
static int uart_id[2] = { -1, -1 };
static void hw_apply(int k)
{
    u32 baud; int bits, par, stop2, dtr, rts; u16 b = uart_base[k];
    serial_params(uart_id[k], &baud, &bits, &par, &stop2, &dtr, &rts);
    u32 div = baud ? 115200 / baud : 12; if (!div) div = 1;
    u8 lcr = (u8)((bits - 5) | (stop2 ? 4 : 0) | (par ? 8 | (par == 2 ? 16 : 0) : 0));
    outb(b + 3, lcr | 0x80); outb(b + 0, (u8)div); outb(b + 1, (u8)(div >> 8)); outb(b + 3, lcr);
    outb(b + 4, (u8)(8 | (dtr ? 1 : 0) | (rts ? 2 : 0)));
}
static void hw_thread(void *arg)
{
    (void)arg;
    for (;;) {
        for (int k = 0; k < 2; k++) {
            if (uart_id[k] < 0) continue;
            u16 b = uart_base[k];
            if (serial_dirty(uart_id[k])) hw_apply(k);
            u8 buf[16]; int n = 0;
            while ((inb(b + 5) & 1) && n < 16) buf[n++] = inb(b);
            if (n) serial_rx(uart_id[k], buf, n);
            u8 t[16]; int c = serial_tx_pull(uart_id[k], t, 16);
            for (int i = 0; i < c; i++) { for (int w = 0; w < 100000 && !(inb(b + 5) & 0x20); w++) ; outb(b, t[i]); }
        }
        thread_sleep_ms(2);
    }
}
void serial_hw_init(void)
{
    int any = 0;
    for (int k = 0; k < 2; k++) {
        u16 b = uart_base[k];
        outb(b + 7, 0xA5);
        if (inb(b + 7) != 0xA5) continue;                              /* no UART here */
        outb(b + 7, 0x5A); if (inb(b + 7) != 0x5A) continue;
        outb(b + 1, 0); outb(b + 2, 0xC7);
        char nm[8]; ksnprintf(nm, sizeof nm, "ttyS%d", k + 2);
        uart_id[k] = serial_register(nm, NULL);
        if (uart_id[k] >= 0) { hw_apply(k); any = 1; }
    }
    if (any) thread_create("uart", 6, hw_thread, NULL);
}
