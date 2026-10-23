/* /dev/input/event0 (keyboard) and event1 (pointer): Linux evdev as the keyboard/mouse drivers report. Qt's evdev plugins, SDL and libinput-less
 * toolkits read these. Keyboards deliver characters (no separate key-up), so every key is a press immediately followed by a release. */
#include "chr.h"
#include "proc.h"
#include "vm.h"
#include "vfs.h"
#include "usb.h"

struct ev { i64 sec, usec; u16 type, code; int value; };           /* struct input_event on 64-bit Linux: 24 bytes */
#define QN 256
struct queue { struct ev q[QN]; volatile u32 h, t, gen; int open; };
static struct queue qs[2];

static void emit(int d, u16 type, u16 code, int value)
{
    struct queue *q = &qs[d];
    if (!q->open || q->h - q->t >= QN) return;
    u64 ns = wall_ns(); struct ev *e = &q->q[q->h % QN];
    e->sec = (i64)(ns / 1000000000ull); e->usec = (i64)(ns % 1000000000ull / 1000); e->type = type; e->code = code; e->value = value;
    q->h++; q->gen++;
}
static void sync_(int d) { emit(d, 0, 0, 0); wake_on(&qs[d], 64); }

/* Linux KEY_* for the characters our keyboard drivers deliver */
static int key_of(int ch, int *shift)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    static const char *shifted_digits = ")!@#$%^&*(";
    *shift = 0;
    if (ch >= 'A' && ch <= 'Z') { *shift = 1; ch += 32; }
    for (int i = 0; row1[i]; i++) if (ch == row1[i]) return 16 + i;
    for (int i = 0; row2[i]; i++) if (ch == row2[i]) return 30 + i;
    for (int i = 0; row3[i]; i++) if (ch == row3[i]) return 44 + i;
    if (ch >= '1' && ch <= '9') return 2 + (ch - '1');
    if (ch == '0') return 11;
    for (int i = 0; shifted_digits[i]; i++) if (ch == shifted_digits[i]) { *shift = 1; return i ? i + 1 : 11; }
    switch (ch) {
    case ' ': return 57; case '\n': case '\r': return 28; case '\b': case 127: return 14; case '\t': return 15; case 0x1b: return 1;
    case '-': return 12; case '_': *shift = 1; return 12; case '=': return 13; case '+': *shift = 1; return 13;
    case '[': return 26; case '{': *shift = 1; return 26; case ']': return 27; case '}': *shift = 1; return 27;
    case ';': return 39; case ':': *shift = 1; return 39; case '\'': return 40; case '"': *shift = 1; return 40; case '`': return 41; case '~': *shift = 1; return 41;
    case '\\': return 43; case '|': *shift = 1; return 43; case ',': return 51; case '<': *shift = 1; return 51; case '.': return 52; case '>': *shift = 1; return 52;
    case '/': return 53; case '?': *shift = 1; return 53;
    case 0x100: return 103; case 0x101: return 108; case 0x102: return 105; case 0x103: return 106;     /* up, down, left, right */
    case 0x104: return 102; case 0x105: return 107; case 0x106: return 111; case 0x107: return 104; case 0x108: return 109;   /* home, end, delete, pgup, pgdn */
    }
    if (ch > 0 && ch < 27) return -(ch - 1 + 'a');                 /* Ctrl-letter: caller maps */
    return 0;
}
void evdev_key(int ch)
{
    int shift, k = key_of(ch, &shift), ctrl = 0;
    if (k < 0) { int l = -k; int dummy; k = key_of(l, &dummy); ctrl = 1; }
    if (k <= 0) return;
    if (ctrl) emit(0, 1, 29, 1);
    if (shift) emit(0, 1, 42, 1);
    emit(0, 1, (u16)k, 1); emit(0, 1, (u16)k, 0);
    if (shift) emit(0, 1, 42, 0);
    if (ctrl) emit(0, 1, 29, 0);
    sync_(0);
}
static int last_btn, last_ax = -1, last_ay = -1;
static void buttons(int btn)
{
    static const u16 codes[3] = { 0x110, 0x111, 0x112 };             /* BTN_LEFT, RIGHT, MIDDLE */
    for (int i = 0; i < 3; i++) if (((btn ^ last_btn) >> i) & 1) emit(1, 1, codes[i], (btn >> i) & 1);
    last_btn = btn;
}
void evdev_mouse_rel(int dx, int dy, int btn)
{
    if (dx) emit(1, 2, 0, dx);
    if (dy) emit(1, 2, 1, dy);
    buttons(btn); sync_(1);
}
void evdev_mouse_abs(int x, int y, int max, int btn)
{
    (void)max;
    if (last_ax >= 0) { if (x - last_ax) emit(1, 2, 0, (x - last_ax) * 1024 / (max ? max : 1)); if (y - last_ay) emit(1, 2, 1, (y - last_ay) * 768 / (max ? max : 1)); }
    last_ax = x; last_ay = y;
    buttons(btn); sync_(1);
}

static int ev_open(int d) { qs[d].open++; return 0; }
static void ev_close(int d) { if (qs[d].open > 0) qs[d].open--; if (!qs[d].open) qs[d].t = qs[d].h; }
static long ev_read(int d, void *buf, size_t n, int nonblock)
{
    struct queue *q = &qs[d];
    if (n < sizeof(struct ev)) return -E_INVAL;
    while (q->h == q->t) {
        if (nonblock) return -E_AGAIN;
        u64 fl = irq_save();
        if (q->h == q->t) sleep_on_locked(q, ~0ull);
        irq_restore(fl);
    }
    size_t c = 0; u8 *o = buf;
    while (c + sizeof(struct ev) <= n && q->h != q->t) { memcpy(o + c, &q->q[q->t % QN], sizeof(struct ev)); q->t++; c += sizeof(struct ev); }
    return (long)c;
}
static int ev_poll(int d, int events) { int r = 0; if ((events & 1) && qs[d].h != qs[d].t) r |= 1; return r; }
static u32 ev_gen(int d) { return qs[d].gen; }

static void setbit(u8 *b, int bit) { b[bit / 8] |= (u8)(1 << (bit % 8)); }
static long ev_ioctl(int d, u64 req, u64 arg, struct proc *p)
{
    u32 dir = (u32)(req >> 30), type = (u32)(req >> 8) & 0xFF, nr = (u32)req & 0xFF, size = (u32)(req >> 16) & 0x3FFF;
    if (type != 'E') return -E_NOTTY;
    u8 bits[96]; memset(bits, 0, sizeof bits);
    if (nr == 0x01 && dir == 2) { int v = 0x010001; return copy_to_user(&p->as, arg, &v, 4) ? -E_FAULT : 0; }              /* EVIOCGVERSION */
    if (nr == 0x02 && dir == 2) { u16 id[4] = { 0x19, 0x1, (u16)(d + 1), 1 }; return copy_to_user(&p->as, arg, id, 8) ? -E_FAULT : 0; }   /* EVIOCGID: BUS_HOST */
    if (nr == 0x06 && dir == 2) {                                                                                         /* EVIOCGNAME */
        const char *nm = d == 0 ? "NEOS keyboard" : "NEOS pointer"; size_t l = strlen(nm) + 1; if (l > size) l = size;
        return copy_to_user(&p->as, arg, nm, l) ? -E_FAULT : (long)l;
    }
    if (nr == 0x07 || nr == 0x08) { char z = 0; return copy_to_user(&p->as, arg, &z, 1) ? -E_FAULT : 1; }                  /* EVIOCGPHYS / UNIQ */
    if (nr == 0x09) return size > 0 ? (copy_to_user(&p->as, arg, bits, size < 8 ? size : 8) ? -E_FAULT : (long)(size < 8 ? size : 8)) : 0;   /* EVIOCGPROP */
    if (nr == 0x90 || nr == 0xA0) return 0;                                                                              /* EVIOCGRAB / SETCLOCKID */
    if (nr >= 0x20 && nr < 0x40 && dir == 2) {                                                                           /* EVIOCGBIT(ev, len) */
        u32 ev = nr - 0x20;
        if (ev == 0) { setbit(bits, 0); if (d == 0) { setbit(bits, 1); } else { setbit(bits, 1); setbit(bits, 2); } }
        else if (ev == 1) { if (d == 0) for (int k = 1; k < 120; k++) setbit(bits, k); else for (int k = 0x110; k <= 0x112; k++) setbit(bits, k); }
        else if (ev == 2 && d == 1) { setbit(bits, 0); setbit(bits, 1); }
        if (size > sizeof bits) size = sizeof bits;
        return copy_to_user(&p->as, arg, bits, size) ? -E_FAULT : (long)size;
    }
    if (nr == 0x18 || nr == 0x19 || nr == 0x1B) { return copy_to_user(&p->as, arg, bits, size < 96 ? size : 96) ? -E_FAULT : 0; }  /* EVIOCGKEY / LED / SW state: nothing pressed */
    return -E_NOTTY;
}
static const struct chrops evops = { .read = ev_read, .poll = ev_poll, .ioctl = ev_ioctl, .gen = ev_gen, .open = ev_open, .close = ev_close };

void evdev_init(void)
{
    vfs_mkdir("/dev/input", 0755);
    chr_register("/dev/input/event0", &evops, 0);
    chr_register("/dev/input/event1", &evops, 1);
}
