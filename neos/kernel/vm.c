#include "vm.h"

#define P_PRESENT 1ull
#define P_WRITE   2ull
#define P_USER    4ull
#define P_NX      (1ull << 63)
#define ADDR_MASK 0x000FFFFFFFFFF000ull

static u64 kern_pml4;
static int smep_on, smap_on;
static struct tunable *t_limit;

u64 vm_kernel_pml4(void) { return kern_pml4; }
int vm_smep(void) { return smep_on; }
int vm_smap(void) { return smap_on; }

static u64 *tbl(u64 phys) { return (u64 *)P2V(phys); }
static u64 new_table(void)
{
    u64 p = pmm_alloc(1);
    if (p) memset(tbl(p), 0, 4096);
    return p;
}

void vm_init(void)
{
    u64 cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    kern_pml4 = cr3 & ADDR_MASK;
    tbl(kern_pml4)[0] = 0;                                   /* the lower half is user space from now on */
    __asm__ volatile("mov %0, %%cr3" :: "r"(kern_pml4) : "memory");
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    u64 cr4; __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (b & (1u << 7)) { cr4 |= 1ull << 20; smep_on = 1; }   /* SMEP: the kernel never executes user pages */
    if (b & (1u << 20)) { cr4 |= 1ull << 21; smap_on = 1; }  /* SMAP: the kernel touches user memory only inside copy_*_user */
    cr4 |= (1ull << 9) | (1ull << 10);                       /* OSFXSR | OSXMMEXCPT: user programs may use SSE */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    u64 cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(1ull << 2)) | (1ull << 1);                /* EM=0, MP=1 */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}

void vm_tunables(void)
{
    module_register("vm", "1.0", "4-level paging, per-process address spaces, SMEP/SMAP, NX");
    t_limit = tune_int("proc", "max_pages", "per-process memory limit (4 KiB pages)", TF_LIVE, 4096, 16, 262144, NULL);
}

void vm_switch(u64 pml4) { __asm__ volatile("mov %0, %%cr3" :: "r"(pml4) : "memory"); }

int vm_new(struct aspace *as, size_t limit)
{
    u64 p = new_table();
    if (!p) return -1;
    u64 *n = tbl(p), *k = tbl(kern_pml4);
    for (int i = 256; i < 512; i++) n[i] = k[i];
    as->pml4 = p; as->pages = 0; as->limit = limit ? limit : (size_t)t_limit->val;
    return 0;
}

static void free_level(u64 phys, int level)           /* level 1 = page table (leaf frames) */
{
    u64 *t = tbl(phys);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & P_PRESENT)) continue;
        if (level == 1) pmm_free(t[i] & ADDR_MASK, 1);
        else free_level(t[i] & ADDR_MASK, level - 1);
    }
    pmm_free(phys, 1);
}

void vm_free(struct aspace *as)
{
    if (!as->pml4) return;
    u64 *p = tbl(as->pml4);
    for (int i = 0; i < 256; i++) if (p[i] & P_PRESENT) free_level(p[i] & ADDR_MASK, 3);
    pmm_free(as->pml4, 1);
    as->pml4 = 0; as->pages = 0;
}

static u64 *pte_of(const struct aspace *as, u64 va, int create)
{
    u64 *t = tbl(as->pml4);
    for (int lvl = 3; lvl >= 1; lvl--) {
        u64 *e = &t[(va >> (12 + 9 * lvl)) & 511];
        if (!(*e & P_PRESENT)) {
            if (!create) return NULL;
            u64 n = new_table();
            if (!n) return NULL;
            *e = n | P_PRESENT | P_WRITE | P_USER;
        }
        t = tbl(*e & ADDR_MASK);
    }
    return &t[(va >> 12) & 511];
}

int vm_alloc(struct aspace *as, u64 va, size_t npages, int vmf)
{
    if (va >= USER_TOP || va + npages * 4096 > USER_TOP || (va & 0xFFF)) return -1;
    for (size_t i = 0; i < npages; i++, va += 4096) {
        u64 *pte = pte_of(as, va, 1);
        if (!pte) return -1;
        if (*pte & P_PRESENT) {                                     /* already there: only widen permissions */
            if (vmf & VMF_W) *pte |= P_WRITE;
            if (vmf & VMF_X) *pte &= ~P_NX;
            continue;
        }
        if (as->pages >= as->limit) return -2;
        u64 f = pmm_alloc(1);
        if (!f) return -1;
        memset(tbl(f), 0, 4096);
        *pte = f | P_PRESENT | P_USER | ((vmf & VMF_W) ? P_WRITE : 0) | ((vmf & VMF_X) ? 0 : P_NX);
        as->pages++;
        __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    }
    return 0;
}

int vm_write(struct aspace *as, u64 va, const void *src, size_t n)
{
    const u8 *s = src;
    while (n) {
        u64 *pte = pte_of(as, va, 0);
        if (!pte || !(*pte & P_PRESENT)) return -1;
        size_t off = va & 0xFFF, c = 4096 - off < n ? 4096 - off : n;
        memcpy((u8 *)tbl(*pte & ADDR_MASK) + off, s, c);
        va += c; s += c; n -= c;
    }
    return 0;
}

int user_range_ok(const struct aspace *as, u64 va, size_t len, int write)
{
    if (!len) return 1;
    if (va >= USER_TOP || len > USER_TOP || va + len > USER_TOP || va + len < va) return 0;
    for (u64 p = va & ~0xFFFull; p < va + len; p += 4096) {
        u64 *pte = pte_of(as, p, 0);
        if (!pte || !(*pte & P_PRESENT) || !(*pte & P_USER) || (write && !(*pte & P_WRITE))) return 0;
    }
    return 1;
}

static inline void stac(void) { if (smap_on) __asm__ volatile("stac" ::: "memory", "cc"); }
static inline void clac(void) { if (smap_on) __asm__ volatile("clac" ::: "memory", "cc"); }

int copy_from_user(const struct aspace *as, void *dst, u64 usrc, size_t n)
{
    if (!user_range_ok(as, usrc, n, 0)) return -1;
    stac(); memcpy(dst, (const void *)usrc, n); clac();
    return 0;
}

int copy_to_user(const struct aspace *as, u64 udst, const void *src, size_t n)
{
    if (!user_range_ok(as, udst, n, 1)) return -1;
    stac(); memcpy((void *)udst, src, n); clac();
    return 0;
}

int strncpy_from_user(const struct aspace *as, char *dst, u64 usrc, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if (i == 0 || ((usrc + i) & 0xFFF) == 0) if (!user_range_ok(as, usrc + i, 1, 0)) return -1;
        char c; stac(); c = *(const char *)(usrc + i); clac();
        dst[i] = c;
        if (!c) return (int)i;
    }
    dst[max - 1] = 0;
    return -1;                                               /* too long */
}
