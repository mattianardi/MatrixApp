#include "kernel.h"

static char brand[49];
static int have_brand, inv_tsc = -1;

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

const char *cpu_brand(void)
{
    if (have_brand) return brand;
    u32 a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        for (u32 i = 0; i < 3; i++) {
            u32 r[4]; cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3]);
            memcpy(brand + i * 16, r, 16);
        }
        brand[48] = 0;
        char *p = brand; while (*p == ' ') p++;
        memmove(brand, p, strlen(p) + 1);
    } else strlcpy(brand, "unknown x86_64 CPU", sizeof brand);
    have_brand = 1;
    return brand;
}

int cpu_invariant_tsc(void)
{
    if (inv_tsc >= 0) return inv_tsc;
    u32 a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    inv_tsc = 0;
    if (a >= 0x80000007) { cpuid(0x80000007, &a, &b, &c, &d); inv_tsc = (d >> 8) & 1; }
    return inv_tsc;
}
