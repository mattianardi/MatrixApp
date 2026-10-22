#include "kernel.h"

#define PAGE 4096
#define MAX_PAGES (1u << 18)            /* 1 GiB, what boot.asm identity-maps */

extern char _kernel_end[];
static u32 bitmap[MAX_PAGES / 32];      /* 1 = used */
static size_t free_count;

static void mark(u64 page, int used)
{
    u32 bit = 1u << (page % 32);
    int was = (bitmap[page / 32] & bit) != 0;
    if (used && !was) { bitmap[page / 32] |= bit; free_count--; }
    if (!used && was) { bitmap[page / 32] &= ~bit; free_count++; }
}

struct mmap_entry { u32 size; u64 addr; u64 len; u32 type; } __attribute__((packed));

void pmm_init(const void *mbi)
{
    for (size_t i = 0; i < MAX_PAGES / 32; i++) bitmap[i] = ~0u;
    free_count = 0;

    const u32 *info = mbi;
    if (!(info[0] & (1 << 6))) panic("bootloader gave no memory map");
    u64 start = ((u64)_kernel_end + PAGE - 1) & ~(u64)(PAGE - 1);
    if (start < 0x200000) start = 0x200000;

    const u8 *p = (const u8 *)(u64)info[12], *end = p + info[11];
    while (p < end) {
        const struct mmap_entry *e = (const void *)p;
        if (e->type == 1) {
            u64 a = e->addr > start ? e->addr : start, b = e->addr + e->len;
            if (b > (u64)MAX_PAGES * PAGE) b = (u64)MAX_PAGES * PAGE;
            for (a = (a + PAGE - 1) & ~(u64)(PAGE - 1); a + PAGE <= b; a += PAGE)
                mark(a / PAGE, 0);
        }
        p += e->size + 4;
    }
}

u64 pmm_alloc(size_t pages)
{
    size_t run = 0;
    for (u64 i = 0; i < MAX_PAGES; i++) {
        run = (bitmap[i / 32] >> (i % 32)) & 1 ? 0 : run + 1;
        if (run == pages) {
            u64 first = i + 1 - pages;
            for (u64 j = first; j <= i; j++) mark(j, 1);
            return first * PAGE;
        }
    }
    return 0;
}

void pmm_free(u64 addr, size_t pages)
{
    for (size_t i = 0; i < pages; i++) mark(addr / PAGE + i, 0);
}

size_t pmm_free_pages(void) { return free_count; }
