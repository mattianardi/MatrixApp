#include "kernel.h"
#include "neos_api.h"
#include "proc.h"

struct idt_entry { u16 lo; u16 sel; u8 ist; u8 attr; u16 mid; u32 hi; u32 zero; } __attribute__((packed));
static struct idt_entry idt[256];
extern u64 isr_table[256];
static void (*irq_fn[16])(void);

void idt_init(void)
{
    for (int i = 0; i < 256; i++) {
        u64 a = isr_table[i];
        u8 attr = i == NEOS_SYSCALL_VECTOR ? 0xEF : 0x8E;      /* ring-3 callable TRAP gate: interrupts stay enabled inside syscalls */
        idt[i] = (struct idt_entry){ a & 0xFFFF, 0x08, 0, attr, (a >> 16) & 0xFFFF, a >> 32, 0 };
    }
    struct { u16 limit; u64 base; } __attribute__((packed)) p = { sizeof idt - 1, (u64)idt };
    __asm__ volatile("lidt %0" :: "m"(p));
}

void pic_init(void)
{
    outb(0x20, 0x11); outb(0xA0, 0x11);     /* ICW1 */
    outb(0x21, 0x20); outb(0xA1, 0x28);     /* ICW2: vectors 0x20.. */
    outb(0x21, 0x04); outb(0xA1, 0x02);     /* ICW3 */
    outb(0x21, 0x01); outb(0xA1, 0x01);     /* ICW4 */
    outb(0x21, 0xFF); outb(0xA1, 0xFF);     /* everything masked; modules unmask what they use */
    irq_unmask(0);
}

void irq_unmask(u8 irq)
{
    if (irq < 8) outb(0x21, inb(0x21) & ~(1 << irq));
    else { outb(0x21, inb(0x21) & ~(1 << 2)); outb(0xA1, inb(0xA1) & ~(1 << (irq - 8))); }
}

void irq_register(u8 irq, void (*fn)(void)) { irq_fn[irq] = fn; irq_unmask(irq); }

void pic_eoi(u8 irq)
{
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static struct frame *isr_dispatch_inner(struct frame *f);
struct frame *isr_dispatch(struct frame *f)
{
    struct frame *r = isr_dispatch_inner(f);
    if (sig_any && (r->cs & 3)) r = signal_deliver(r);              /* about to return to user mode: run a pending signal handler first */
    return r;
}

static struct frame *isr_dispatch_inner(struct frame *f)
{
    if (f->vec < 32) {
        if (f->cs & 3) return proc_fault(f);                   /* a user program misbehaved: kill it, not the kernel */
        u64 cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        { u64 *sp = (u64 *)f->rsp; if (f->rsp > PHYSMAP) kprintf("[fault] rsp=%p stack: %lx %lx %lx %lx %lx (thread %d)\n", f->rsp, sp[0], sp[1], sp[2], sp[3], sp[4], thread_current_id()); }
        panic("exception %lu err=%lx rip=%p cr2=%p", f->vec, f->err, f->rip, cr2);
    }
    if (f->vec < 48) {
        u8 irq = (u8)(f->vec - 32);
        pic_eoi(irq);
        if (irq == 0) {
            u64 t0 = rdtsc();
            timer_tick();
            alarm_tick();
            struct frame *n = sched_on_tick(f);
            timer_isr_account(rdtsc() - t0);
            return n;
        }
        if (irq_fn[irq]) irq_fn[irq]();
        return f;
    }
    if (f->vec == 0x100) {                                       /* SYSCALL instruction: Linux ABI */
        __asm__ volatile("sti");
        struct frame *r = linux_syscall(f);
        __asm__ volatile("cli");
        return r;
    }
    if (f->vec == NEOS_YIELD_VECTOR) return sched_on_yield(f);
    if (f->vec == NEOS_SYSCALL_VECTOR) {
        if (f->cs & 3) return user_syscall(f);
        f->rax = (u64)neos_syscall(f->rax, f->rdi, f->rsi, f->rdx, f->r10);
        return f;
    }
    return f;   /* spurious */
}
