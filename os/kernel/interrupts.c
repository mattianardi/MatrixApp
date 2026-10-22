#include "kernel.h"

struct idt_entry { u16 lo; u16 sel; u8 ist; u8 attr; u16 mid; u32 hi; u32 zero; } __attribute__((packed));
static struct idt_entry idt[256];
extern u64 isr_table[256];

volatile u64 ticks;

void idt_init(void)
{
    for (int i = 0; i < 256; i++) {
        u64 a = isr_table[i];
        idt[i] = (struct idt_entry){ a & 0xFFFF, 0x08, 0, 0x8E, (a >> 16) & 0xFFFF, a >> 32, 0 };
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
    outb(0x21, 0xFE); outb(0xA1, 0xFF);     /* only IRQ0 (timer) unmasked */
}

void pic_eoi(u8 irq)
{
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void pit_init(u32 hz)
{
    u32 div = 1193182 / hz;
    outb(0x43, 0x34);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
}

struct frame *isr_dispatch(struct frame *f)
{
    if (f->vec < 32) {
        u64 cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        panic("exception %lu err=%lx rip=%p cr2=%p", f->vec, f->err, f->rip, cr2);
    }
    if (f->vec == 0x20) {
        pic_eoi(0);
        ticks++;
        return sched_on_tick(f);
    }
    if (f->vec == 0x81) return sched_on_yield(f);
    return f;   /* spurious */
}
