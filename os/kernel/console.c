#include "kernel.h"

#define COM1 0x3F8
static u16 *const vga = (u16 *)0xB8000;
static int cx, cy;

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void console_init(void)
{
    serial_init();
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x0720;
}

static void vga_putc(char c)
{
    if (c == '\n') { cx = 0; cy++; }
    else if (c != '\r') { vga[cy * 80 + cx] = 0x0700 | (u8)c; if (++cx == 80) { cx = 0; cy++; } }
    if (cy == 25) {
        memmove(vga, vga + 80, 80 * 24 * 2);
        for (int i = 0; i < 80; i++) vga[24 * 80 + i] = 0x0720;
        cy = 24;
    }
}

void kputc(char c)
{
    while (!(inb(COM1 + 5) & 0x20)) { }
    if (c == '\n') { outb(COM1, '\r'); while (!(inb(COM1 + 5) & 0x20)) { } }
    outb(COM1, (u8)c);
    vga_putc(c);
}

static void put_num(u64 v, unsigned base, int width, char pad, int neg)
{
    char buf[24]; int n = 0;
    do { buf[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    if (neg) buf[n++] = '-';
    for (int i = n; i < width; i++) kputc(pad);
    while (n) kputc(buf[--n]);
}

static void kvprintf(const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { kputc(*f); continue; }
        f++;
        char pad = ' '; int width = 0, lng = 0;
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        switch (*f) {
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); while (s && *s) kputc(*s++); break; }
        case 'd': { long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
                    put_num(v < 0 ? -(u64)v : (u64)v, 10, width, pad, v < 0); break; }
        case 'u': put_num(lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 10, width, pad, 0); break;
        case 'x': put_num(lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 16, width, pad, 0); break;
        case 'p': kputc('0'); kputc('x'); put_num(va_arg(ap, u64), 16, 16, '0', 0); break;
        case '%': kputc('%'); break;
        default:  kputc('%'); kputc(*f); break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); kvprintf(fmt, ap); va_end(ap);
}

void qemu_exit(u8 code)
{
    outb(0xF4, code);           /* isa-debug-exit: status = (code << 1) | 1 */
    for (;;) __asm__ volatile("cli; hlt");
}

void panic(const char *fmt, ...)
{
    __asm__ volatile("cli");
    kprintf("\n*** KERNEL PANIC: ");
    va_list ap; va_start(ap, fmt); kvprintf(fmt, ap); va_end(ap);
    kputc('\n');
    qemu_exit(0x11);
}

void *memset(void *d, int c, size_t n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    u8 *p = d; const u8 *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
