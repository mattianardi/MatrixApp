#include "kernel.h"

/*
 * Segregated power-of-two free lists over a bump arena: kmalloc/kfree are O(1)
 * with a fixed worst case, so they are usable from time-critical code (there is
 * no coalescing, memory of a size class is only reused by that class).
 */
#define CLASSES 24
#define HDR 16
struct fnode { struct fnode *next; };
static u8 *bump, *arena_end;
static struct fnode *freel[CLASSES];
static size_t used, peak, total;
static struct tunable *t_mb;

void heap_init(void)
{
    module_register("heap", "1.0", "O(1) size-class kernel allocator");
    t_mb = tune_int("heap", "size_mb", "kernel heap size (MiB)", TF_BOOT, 64, 1, 256, NULL);
    size_t pages = (size_t)t_mb->val * 256;
    u64 a = pmm_alloc(pages);
    if (!a) panic("cannot allocate %lu MiB kernel heap", (u64)t_mb->val);
    bump = P2V(a); arena_end = bump + pages * 4096; total = pages * 4096;
}

static int class_of(size_t n) { int k = 0; size_t s = 16; while (s < n) { s <<= 1; k++; } return k; }

void *kmalloc(size_t n)
{
    int k = class_of(n);
    if (k >= CLASSES) return NULL;
    size_t sz = (size_t)16 << k;
    u64 fl = irq_save();
    u8 *blk = NULL;
    if (freel[k]) { blk = (u8 *)freel[k] - HDR; freel[k] = freel[k]->next; }
    else if (bump + HDR + sz <= arena_end) { blk = bump; bump += HDR + sz; }
    if (blk) { *(u64 *)blk = (u64)k; used += sz; if (used > peak) peak = used; }
    irq_restore(fl);
    return blk ? blk + HDR : NULL;
}

void kfree(void *p)
{
    if (!p) return;
    u64 fl = irq_save();
    int k = (int)*(u64 *)((u8 *)p - HDR);
    struct fnode *f = p; f->next = freel[k]; freel[k] = f;
    used -= (size_t)16 << k;
    irq_restore(fl);
}

void heap_stats(size_t *u, size_t *p, size_t *t) { *u = used; *p = peak; *t = total; }
