#include "kernel.h"
const char *dbg_free_what = "?"; int dbg_free_pid;

#define PAGE 4096
#define MAX_PAGES (1u << 18)            /* 1 GiB, what boot.asm identity-maps */

extern char _kernel_end[];     /* virtual (higher half) */
static u32 bitmap[MAX_PAGES / 32];      /* 1 = used */
static size_t free_count, total_count;

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
    free_count = total_count = 0;
    module_register("pmm", "1.0", "physical page allocator (bitmap)");

    const u32 *info = mbi;
    if (!(info[0] & (1 << 6))) panic("bootloader gave no memory map");
    u64 start = ((V2P(_kernel_end)) + PAGE - 1) & ~(u64)(PAGE - 1);
    if (start < 0x200000) start = 0x200000;

    const u8 *p = P2V(info[12]), *end = p + info[11];
    const u32 *mods = (info[0] & (1 << 3)) ? P2V(info[6]) : NULL;      /* boot modules (initrd) must not be handed out */
    u32 nmods = mods ? info[5] : 0;
    while (p < end) {
        const struct mmap_entry *e = (const void *)p;
        if (e->type == 1) {
            u64 a = e->addr > start ? e->addr : start, b = e->addr + e->len;
            if (b > (u64)MAX_PAGES * PAGE) b = (u64)MAX_PAGES * PAGE;
            for (a = (a + PAGE - 1) & ~(u64)(PAGE - 1); a + PAGE <= b; a += PAGE)
                { mark(a / PAGE, 0); total_count++; }
        }
        p += e->size + 4;
    }
    for (u32 i = 0; i < nmods; i++)
        for (u64 a = mods[i * 4] & ~0xFFFull; a < mods[i * 4 + 1]; a += PAGE) { if (!(bitmap[(a / PAGE) / 32] & (1u << ((a / PAGE) % 32)))) { mark(a / PAGE, 1); total_count--; } }
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
    for (size_t i = 0; i < pages; i++) {
        u64 pg = addr / PAGE + i;
        if (!(bitmap[pg / 32] & (1u << (pg % 32)))) panic("pmm: double free of physical page %lx (caller %p, free context: %s pid %d)", pg * PAGE, __builtin_return_address(0), dbg_free_what, dbg_free_pid);
        mark(pg, 0);
    }
}

size_t pmm_free_pages(void) { return free_count; }
size_t pmm_total_pages(void) { return total_count; }
