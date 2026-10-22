#include "vm.h"

struct tss { u32 res0; u64 rsp0, rsp1, rsp2, res1, ist[7], res2; u16 res3, iomap; } __attribute__((packed));
static u64 gdt[7] __attribute__((aligned(16)));
static struct tss tss;

u64 syscall_kstack;
void tss_set_rsp0(u64 rsp0) { tss.rsp0 = rsp0; syscall_kstack = rsp0; }

extern void syscall_entry(void);
static void wrmsr_(u32 msr, u64 v) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32))); }
static u64 rdmsr_(u32 msr) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return ((u64)hi << 32) | lo; }

void syscall_init(void)
{
    wrmsr_(0xC0000080, rdmsr_(0xC0000080) | 1);                /* EFER.SCE */
    wrmsr_(0xC0000081, (0x10ull << 48) | (0x08ull << 32));      /* STAR */
    wrmsr_(0xC0000082, (u64)syscall_entry);                      /* LSTAR */
    wrmsr_(0xC0000084, 0x47700);                                 /* SFMASK: clear IF DF TF AC NT IOPL */
}

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull;      /* 0x08 kernel code, 64-bit */
    gdt[2] = 0x00CF92000000FFFFull;      /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFull;      /* 0x18 user data   (SYSRET layout: data = code - 8) */
    gdt[4] = 0x00AFFA000000FFFFull;      /* 0x20 user code, 64-bit */
    u64 base = (u64)&tss, lim = sizeof tss - 1;
    gdt[5] = (lim & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) | (((lim >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
    tss.iomap = sizeof tss;              /* no I/O permission bitmap: ring 3 gets no port access */
    struct { u16 limit; u64 base; } __attribute__((packed)) p = { sizeof gdt - 1, (u64)gdt };
    __asm__ volatile("lgdt %0" :: "m"(p));
    __asm__ volatile(
        "pushq $0x08\n lea 1f(%%rip), %%rax\n pushq %%rax\n lretq\n1:\n"
        "mov $0x10, %%ax\n mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%ss\n"
        "xor %%eax, %%eax\n mov %%ax, %%fs\n mov %%ax, %%gs\n"
        ::: "rax", "memory");
    __asm__ volatile("ltr %%ax" :: "a"((u16)0x28));
}
