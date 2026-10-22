#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define NEOS_NAME    "NEOS"
#define NEOS_VERSION "0.2.0"
#define NEOS_AUTHOR  "Mattia Nardi"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;

/* Register state saved by isr_common; layout must match isr.asm. */
struct frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vec, err;
    u64 rip, cs, rflags, rsp, ss;
};

/* ---- address space layout ---- */
#define KERNEL_VMA 0xFFFFFFFF80000000ull      /* kernel image: virt = phys + KERNEL_VMA */
#define PHYSMAP    0xFFFF800000000000ull      /* all RAM and MMIO: virt = phys + PHYSMAP */
static inline void *P2V(u64 phys) { return (void *)(phys + PHYSMAP); }
static inline u64 V2P(const void *v)
{
    u64 a = (u64)v;
    return a >= KERNEL_VMA ? a - KERNEL_VMA : a - PHYSMAP;
}

/* ---- port I/O, interrupt flag ---- */
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8   inb(u16 p)         { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline u16  inw(u16 p)         { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline u32  inl(u16 p)         { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u64 rdtsc(void)         { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
static inline u64 irq_save(void)      { u64 f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_restore(u64 f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

/* ---- lib.c ---- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strstr(const char *h, const char *n);
char  *strrchr(const char *s, int c);
char  *strcpy(char *d, const char *s);
char  *strchr(const char *s, int c);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
/* neofs.c: durable storage */
void neofs_init(void); void neofs_start(void); int neofs_load(void); int neofs_sync(int force); int neofs_format(void);
int neofs_save_settings(void); void neofs_status(void); int neofs_present(void);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    parse_i64(const char *s, i64 *out);
int    b64_encode(const void *in, size_t n, char *out, size_t on);   /* returns length or -1 */
int    b64_decode(const char *in, void *out, size_t on);             /* returns bytes or -1 */          /* dec / 0x hex, optional sign; 0 = ok */
typedef void (*fmt_out)(char c, void *ctx);
void   kvformat(fmt_out out, void *ctx, const char *fmt, va_list ap);
int    ksnprintf(char *buf, size_t n, const char *fmt, ...);
int    kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
static inline int is_digit(int c) { return c >= '0' && c <= '9'; }
static inline int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int to_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* ---- console.c: async log ring -> serial + VGA (ANSI subset) ---- */
void console_init(void);
void console_async_start(void);     /* from now on kputc only appends to the ring */
void console_flush_sync(void);
void kputc(char c);
void kprintf(const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((noreturn));
void qemu_exit(u8 code) __attribute__((noreturn));
void console_set_top(int row);      /* first VGA row of the scrolling stream */
void console_clear_stream(void);
void vga_write(int row, int col, const char *s, u8 attr, int width);   /* fixed-position text (UI) */
void console_dump_screen(void);     /* debug: print VGA text to serial */
u64  console_dropped(void);
u16 *console_cells(void);           /* the 80x25 text cell grid (char | attr<<8) */
void console_cursor(int *row, int *col);
void console_use_cells(u16 *cells); /* switch the grid to RAM (the graphical workspace draws it) */
#define VGA_ROWS 25
#define VGA_COLS 80

/* ---- timer.c ---- */
extern volatile u64 ticks;
void   timer_init(void);
void   timer_set_hz(u32 hz);
u32    timer_hz(void);
void   timer_tick(void);            /* called from the IRQ0 handler */
u64    time_ns(void);               /* monotonic, TSC-interpolated once calibrated */
u64    tsc_hz(void);
u64    ms_to_ticks(u64 ms);
void   timer_jitter(u64 *min_ns, u64 *max_ns, u64 *avg_ns, u64 *n);
void   timer_jitter_reset(void);
void   timer_isr_account(u64 cycles);
void   timer_isr_stats(u64 *avg_cycles, u64 *max_cycles, u64 *n);
u64    cycles_to_ns(u64 cycles);

/* ---- interrupts.c ---- */
void idt_init(void);
void pic_init(void);
void pic_eoi(u8 irq);
void irq_unmask(u8 irq);
void irq_register(u8 irq, void (*fn)(void));

/* ---- memory ---- */
void   pmm_init(const void *mbi);
u64    pmm_alloc(size_t pages);          /* contiguous, 0 on failure */
void   pmm_free(u64 addr, size_t pages);
size_t pmm_free_pages(void);
size_t pmm_total_pages(void);
void   heap_init(void);
void  *kmalloc(size_t n);                /* O(1) size-class allocator */
void   kfree(void *p);
void   heap_stats(size_t *used, size_t *peak, size_t *size);

/* ---- tune.c: run-time / boot-time settings registry ---- */
enum { TT_INT, TT_BOOL, TT_STR };
#define TF_LIVE   1     /* changes apply immediately */
#define TF_BOOT   2     /* stored as pending, applies at the next boot (boot args / saved config) */
#define TF_RO     4
#define TF_NOAGENT 8    /* never modifiable by an agent, whatever its capabilities */
struct tunable;
typedef int (*tune_cb)(struct tunable *t, i64 newval, const char *newstr);   /* 0 = accept */
struct tunable {
    char name[40]; const char *module; const char *desc;
    u8 type, flags;
    i64 val, pending, def, min, max;
    char sval[48], spend[48];
    int has_pending;
    tune_cb cb;
};
struct tunable *tune_int (const char *mod, const char *name, const char *desc, u8 flags, i64 def, i64 min, i64 max, tune_cb cb);
struct tunable *tune_bool(const char *mod, const char *name, const char *desc, u8 flags, int def, tune_cb cb);
struct tunable *tune_str (const char *mod, const char *name, const char *desc, u8 flags, const char *def, tune_cb cb);
struct tunable *tune_find(const char *full);
int    tune_count(void);
struct tunable *tune_at(int i);
/* 0 = applied, 1 = stored for next boot, <0 = error (*msg set) */
int    tune_set(const char *full, const char *text, const char **msg);
void   tune_format(const struct tunable *t, char *buf, size_t n);          /* current value as text */
void   tune_bootargs(const char *cmdline);       /* parse "neos.<name>=<value>" tokens */
void   tune_bootarg_add(const char *name, const char *val);
int    tune_save_text(char *buf, size_t n);      /* "name=value\n" for all non-default settings */

/* ---- module.c ---- */
void   module_register(const char *name, const char *version, const char *desc);
int    module_count(void);
void   module_get(int i, const char **name, const char **version, const char **desc);

/* ---- splash.c / cpu.c ---- */
void splash_info(const char *label, void (*fmt)(char *buf, size_t n));
void splash_show(void);
const char *cpu_brand(void);
int  cpu_has_rdrand(void);
int  hw_random(u64 *v);
int  cpu_invariant_tsc(void);

/* ---- sched.c: static-priority preemptive, 0 = highest ---- */
#define SCHED_MAX_THREADS 32
#define SCHED_PRIO_IDLE   7
typedef void (*thread_fn)(void *);
struct thread_info { int id; const char *name; const char *state; int prio; u64 runticks; int stack_kib; u64 lat_max_ns; };
void sched_init(void);
int  thread_create(const char *name, u8 prio, thread_fn fn, void *arg);
/* defined after struct uctx */
void *sched_current_proc(void);
void thread_mark_dead(int id);
int  thread_is_dead(int id);
int  thread_self(void);
void thread_yield(void);
void thread_sleep_until(u64 tick);
void thread_sleep_ms(u64 ms);
int  thread_set_prio(int id, int prio);
int  thread_kill(int id);
int  thread_info(int id, struct thread_info *out);   /* 0 if slot used */
struct frame *sched_on_tick(struct frame *f);
struct frame *sched_on_yield(struct frame *f);
void sched_rescale(u32 old_hz, u32 new_hz);
void sched_register_tunables(void);
u64  sched_runticks_total(void);
int  thread_protected(int id);                       /* core system threads agents may never touch */
void sched_latency(u64 *max_ns, u64 *avg_ns, u64 *n);   /* ready->running latency of real-time-class threads */
void sched_latency_reset(void);

/* per-thread user context (Linux ABI): TLS base, CLONE_CHILD_CLEARTID address, user-visible thread id */
struct uctx { u64 fsbase, gsbase, clear_tid; int utid; int last_sys; };
struct uctx *uctx_current(void);
struct uctx *uctx_of(int id);
int  thread_current_id(void);
/* wait queues by key. sleep_on_locked: caller holds irq_save and has just re-checked its condition,
 * which makes check+sleep atomic against wake_on (single CPU). timeout_tick ~0 = forever. */
void sleep_on_locked(void *key, u64 timeout_tick);
int  wake_on(void *key, int max);
int  thread_create_user(const char *name, u8 prio, void *proc, u64 cr3, u64 rip, u64 usp, const struct uctx *uc);
int  thread_create_user_frame(const char *name, u8 prio, void *proc, u64 cr3, const struct frame *init, const struct uctx *uc);

void sched_switch_cr3(u64 cr3);
void sched_set_gsbase(u64 v);        /* Windows programs keep their TEB here */
void sched_dump(void);               /* Ctrl-T: every thread, what it waits on, its last syscall */
void sched_kill_threads_of(void *proc);
void sched_kill_threads_of_except(void *proc, int keep);
int  sched_threads_alive(void *proc);
int  thread_prio_of(int id);
u64  wall_ns(void);                  /* CLOCK_REALTIME */
void fill_random(void *buf, size_t n);

/* ---- selftest.c ---- */
int  selftest_run(void);     /* 0 = pass; blocks the calling thread */
