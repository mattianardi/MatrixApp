#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* Register state saved by isr_common; layout must match isr.asm. */
struct frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vec, err;
    u64 rip, cs, rflags, rsp, ss;
};

static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* console / panic */
void console_init(void);
void kputc(char c);
void kprintf(const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((noreturn));
void qemu_exit(u8 code) __attribute__((noreturn));

/* libc-ish helpers the compiler may call */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);

/* interrupts / timer */
#define TICK_HZ 1000
void idt_init(void);
void pic_init(void);
void pit_init(u32 hz);
void pic_eoi(u8 irq);
extern volatile u64 ticks;

/* physical memory (identity mapped, first 1 GiB) */
void pmm_init(const void *mbi);
u64  pmm_alloc(size_t pages);          /* contiguous, returns 0 on failure */
void pmm_free(u64 addr, size_t pages);
size_t pmm_free_pages(void);

/* scheduler: static-priority preemptive, 0 = highest, round-robin within a level */
#define SCHED_MAX_THREADS 32
#define SCHED_PRIO_IDLE   7
#define SCHED_QUANTUM     10
typedef void (*thread_fn)(void *);
void sched_init(void);
int  thread_create(const char *name, u8 prio, thread_fn fn, void *arg);
void thread_yield(void);
void thread_sleep_until(u64 tick);
struct frame *sched_on_tick(struct frame *f);
struct frame *sched_on_yield(struct frame *f);
