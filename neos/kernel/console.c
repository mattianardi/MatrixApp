#include "kernel.h"

/*
 * Output path: kputc() appends to a ring buffer (bounded, O(1), never touches
 * hardware), a low-priority thread drains it to serial + VGA. Real-time threads
 * therefore never wait for a slow serial port. Before the scheduler runs (and
 * in panic) output is synchronous.
 */
#define COM1 0x3F8
#define LOG_SZ 65536
#define LOG_MASK (LOG_SZ - 1)

static char ring[LOG_SZ];
static volatile u32 head, tail;
static volatile u64 dropped;
static int async;
static struct tunable *t_serial, *t_vga, *t_batch, *t_color;

static u16 *const vga = (u16 *)0xB8000;
static int vtop, vrow, vcol;
static u8 vattr = 0x07;
static int esc, esc_n, esc_have;
static int esc_arg[6];

static void serial_putc(char c)
{
    for (int i = 0; i < 20000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, (u8)c);
}

static void vga_cursor(void)
{
    u16 pos = (u16)(vrow * VGA_COLS + vcol);
    outb(0x3D4, 14); outb(0x3D5, pos >> 8);
    outb(0x3D4, 15); outb(0x3D5, pos & 0xFF);
}

static void vga_scroll(void)
{
    if (vrow < VGA_ROWS) return;
    memmove(vga + vtop * VGA_COLS, vga + (vtop + 1) * VGA_COLS, (VGA_ROWS - 1 - vtop) * VGA_COLS * 2);
    for (int i = 0; i < VGA_COLS; i++) vga[(VGA_ROWS - 1) * VGA_COLS + i] = 0x0720;
    vrow = VGA_ROWS - 1;
}

static void vga_sgr(int p)
{
    static const u8 map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };    /* ANSI -> VGA colour index */
    if (p == 0) vattr = 0x07;
    else if (p == 1) vattr |= 0x08;
    else if (p >= 30 && p <= 37) vattr = (vattr & 0xF8) | map[p - 30];
    else if (p >= 90 && p <= 97) vattr = (vattr & 0xF0) | 0x08 | map[p - 90];
    else if (p >= 40 && p <= 47) vattr = (vattr & 0x8F) | (map[p - 40] << 4);
}

static void vga_csi(char fin)
{
    int n = esc_have ? esc_n + 1 : 0, a0 = n ? esc_arg[0] : 0;
    switch (fin) {
    case 'm': if (!n) vga_sgr(0); for (int i = 0; i < n; i++) vga_sgr(esc_arg[i]); break;
    case 'K': for (int i = vcol; i < VGA_COLS; i++) vga[vrow * VGA_COLS + i] = ((u16)vattr << 8) | ' '; break;
    case 'D': vcol -= a0 ? a0 : 1; if (vcol < 0) vcol = 0; break;
    case 'C': vcol += a0 ? a0 : 1; if (vcol > VGA_COLS - 1) vcol = VGA_COLS - 1; break;
    case 'J': console_clear_stream(); break;
    case 'H': vrow = vtop; vcol = 0; break;
    }
}

static void vga_stream(char c)
{
    if (esc == 1) { if (c == '[') { esc = 2; esc_n = 0; esc_have = 0; memset(esc_arg, 0, sizeof esc_arg); } else esc = 0; return; }
    if (esc == 2) {
        if (is_digit(c)) { esc_arg[esc_n] = esc_arg[esc_n] * 10 + (c - '0'); esc_have = 1; }
        else if (c == ';') { if (esc_n < 5) esc_n++; esc_have = 1; }
        else { vga_csi(c); esc = 0; }
        return;
    }
    if (c == 0x1B) { esc = 1; return; }
    if (c == '\n') { vcol = 0; vrow++; }
    else if (c == '\r') vcol = 0;
    else if (c == '\b') { if (vcol) vcol--; }
    else if (c == '\t') { vcol = (vcol + 8) & ~7; if (vcol >= VGA_COLS) { vcol = 0; vrow++; } }
    else if ((u8)c >= 32) {
        vga[vrow * VGA_COLS + vcol] = ((u16)vattr << 8) | (u8)c;
        if (++vcol == VGA_COLS) { vcol = 0; vrow++; }
    }
    vga_scroll();
}

static void emit(char c)
{
    if (!t_serial || t_serial->val) {
        if (c == '\n') serial_putc('\r');
        serial_putc(c);
    }
    if (!t_vga || t_vga->val) vga_stream(c);
}

void kputc(char c)
{
    if (!async) { emit(c); return; }
    u64 fl = irq_save();
    if (((head + 1) & LOG_MASK) == tail) dropped++;
    else { ring[head] = c; head = (head + 1) & LOG_MASK; }
    irq_restore(fl);
}

void console_flush_sync(void)
{
    while (tail != head) { emit(ring[tail]); tail = (tail + 1) & LOG_MASK; }
    vga_cursor();
}

static void console_thread(void *arg)
{
    (void)arg;
    for (;;) {
        i64 n = 0;
        while (tail != head && n < t_batch->val) { emit(ring[tail]); tail = (tail + 1) & LOG_MASK; n++; }
        vga_cursor();
        thread_sleep_until(ticks + 1);
    }
}

void console_init(void)
{
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01); outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7); outb(COM1 + 4, 0x0B);   /* 115200 8N1, FIFO */
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++) vga[i] = 0x0720;
    module_register("console", "1.0", "async log ring, serial + VGA(ANSI) sinks");
    t_serial = tune_bool("console", "serial",   "mirror output to COM1", TF_LIVE, 1, NULL);
    t_vga    = tune_bool("console", "vga",      "mirror output to VGA text screen", TF_LIVE, 1, NULL);
    t_batch  = tune_int ("console", "batch",    "max bytes drained per tick by the console thread", TF_LIVE, 512, 16, 65536, NULL);
    t_color  = tune_bool("console", "color",    "emit ANSI colour sequences", TF_LIVE, 1, NULL);
}

void console_async_start(void)
{
    console_flush_sync();
    async = 1;
    thread_create("console", 6, console_thread, NULL);
}

void console_set_top(int row) { vtop = row; if (vrow < vtop) { vrow = vtop; vcol = 0; } }

void console_clear_stream(void)
{
    for (int r = vtop; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++) vga[r * VGA_COLS + c] = 0x0720;
    vrow = vtop; vcol = 0;
}

void vga_write(int row, int col, const char *s, u8 attr, int width)
{
    int i = 0;
    for (; s && s[i] && i < width && col + i < VGA_COLS; i++) vga[row * VGA_COLS + col + i] = ((u16)attr << 8) | (u8)s[i];
    for (; i < width && col + i < VGA_COLS; i++) vga[row * VGA_COLS + col + i] = ((u16)attr << 8) | ' ';
}

void console_dump_screen(void)
{
    console_flush_sync();
    for (int r = 0; r < VGA_ROWS; r++) {
        char line[VGA_COLS + 1]; int n = VGA_COLS;
        for (int c = 0; c < VGA_COLS; c++) line[c] = (char)(vga[r * VGA_COLS + c] & 0xFF);
        while (n && line[n - 1] == ' ') n--;
        serial_putc('|');
        for (int c = 0; c < n; c++) serial_putc(line[c]);
        serial_putc('\r'); serial_putc('\n');
    }
}

u64 console_dropped(void) { return dropped; }

static void kfmt_out(char c, void *ctx)
{
    (void)ctx;
    if (!t_color || t_color->val) { kputc(c); return; }
    /* colour disabled: swallow SGR sequences */
    static int st;
    if (st == 0 && c == 0x1B) { st = 1; return; }
    if (st == 1) { if (c == '[') { st = 2; return; } st = 0; kputc(0x1B); kputc(c); return; }
    if (st == 2) { if (c == 'm') st = 0; else if (!is_digit(c) && c != ';') { st = 0; kputc(0x1B); kputc('['); kputc(c); } return; }
    kputc(c);
}

void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); kvformat(kfmt_out, NULL, fmt, ap); va_end(ap);
}

void qemu_exit(u8 code)
{
    console_flush_sync();
    outb(0xF4, code);           /* isa-debug-exit: status = (code << 1) | 1 */
    for (;;) __asm__ volatile("cli; hlt");
}

void panic(const char *fmt, ...)
{
    __asm__ volatile("cli");
    async = 0;
    console_flush_sync();
    kprintf("\n\x1b[1;31m*** NEOS KERNEL PANIC:\x1b[0m ");
    va_list ap; va_start(ap, fmt); kvformat(kfmt_out, NULL, fmt, ap); va_end(ap);
    kputc('\n');
    qemu_exit(0x11);
}
