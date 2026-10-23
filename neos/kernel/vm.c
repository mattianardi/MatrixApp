#include "vm.h"
#include "vfs.h"

#define P_PRESENT 1ull
#define P_WRITE   2ull
#define P_USER    4ull
#define P_SHARED  (1ull << 9)                    /* software bit: frame belongs to a struct shm, never freed with the page table */
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
    t_limit = tune_int("proc", "max_pages", "per-process memory limit (4 KiB pages)", TF_LIVE, 65536, 16, 262144, NULL);
}

void vm_switch(u64 pml4) { __asm__ volatile("mov %0, %%cr3" :: "r"(pml4) : "memory"); }

int vm_new(struct aspace *as, size_t limit)
{
    u64 p = new_table();
    if (!p) return -1;
    u64 *n = tbl(p), *k = tbl(kern_pml4);
    for (int i = 256; i < 512; i++) n[i] = k[i];
    as->pml4 = p; as->pages = 0; as->nvma = 0; as->limit = limit ? limit : (size_t)t_limit->val;
    return 0;
}

static void free_level(u64 phys, int level)           /* level 1 = page table (leaf frames) */
{
    u64 *t = tbl(phys);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & P_PRESENT)) continue;
        if (level == 1) { if (!(t[i] & P_SHARED)) pmm_free(t[i] & ADDR_MASK, 1); }
        else free_level(t[i] & ADDR_MASK, level - 1);
    }
    pmm_free(phys, 1);
}

/* ------------------------------------------------------------ shared memory objects */
struct shm { int refs; struct inode *ino; u64 npages; u64 *pg; };
#define SHM_MAX 128
static struct shm *shm_reg[SHM_MAX];

struct shm *shm_get(struct inode *ino, u64 npages)
{
    u64 fl = irq_save();
    int slot = -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm_reg[i] && ino && shm_reg[i]->ino == ino) { slot = i; break; }
        if (!shm_reg[i] && slot < 0) slot = i;
    }
    irq_restore(fl);
    if (slot < 0) return NULL;
    struct shm *s = shm_reg[slot];
    if (s && ino && s->ino == ino) {
        if (npages > s->npages) {
            u64 *np = kmalloc(npages * 8); if (!np) return NULL;
            memset(np, 0, npages * 8); memcpy(np, s->pg, s->npages * 8);
            u64 *old = s->pg; s->pg = np; s->npages = npages; kfree(old);
        }
        s->refs++; return s;
    }
    s = kmalloc(sizeof *s); if (!s) return NULL;
    s->pg = kmalloc(npages * 8); if (!s->pg) { kfree(s); return NULL; }
    memset(s->pg, 0, npages * 8); s->refs = 1; s->ino = ino; s->npages = npages; if (ino) inode_ref(ino);
    fl = irq_save(); shm_reg[slot] = s; irq_restore(fl);
    return s;
}

void shm_sync(struct shm *s)
{
    if (!s->ino) return;
    for (u64 i = 0; i < s->npages; i++) {
        if (!s->pg[i] || i * 4096 >= s->ino->size) continue;
        u64 left = s->ino->size - i * 4096;
        inode_write(s->ino, i * 4096, tbl(s->pg[i]), left < 4096 ? left : 4096);
    }
}

static void shm_release(struct shm *s)
{
    shm_sync(s);
    if (--s->refs > 0) return;
    for (int i = 0; i < SHM_MAX; i++) if (shm_reg[i] == s) shm_reg[i] = NULL;
    for (u64 i = 0; i < s->npages; i++) if (s->pg[i]) pmm_free(s->pg[i], 1);
    if (s->ino) inode_unref(s->ino);
    kfree(s->pg); kfree(s);
}

u64 shm_key(const struct vma *v, u64 addr) { return ((u64)(uintptr_t)v->shm >> 4) * 1315423911u + ((v->off + addr - v->start) >> 2); }

void vm_free(struct aspace *as)
{
    if (!as->pml4) return;
    for (int i = 0; i < as->nvma; i++) if (as->vma[i].shm) shm_release(as->vma[i].shm);
    as->nvma = 0;
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
        if (!pte || !(*pte & P_PRESENT)) { if (vm_fault(as, va & ~0xFFFull, 1, 0)) return -1; pte = pte_of(as, va, 0); }
        if (!pte || !(*pte & P_PRESENT)) return -1;
        size_t off = va & 0xFFF, c = 4096 - off < n ? 4096 - off : n;
        memcpy((u8 *)tbl(*pte & ADDR_MASK) + off, s, c);
        va += c; s += c; n -= c;
    }
    return 0;
}

int user_range_ok(const struct aspace *as_c, u64 va, size_t len, int write)
{
    struct aspace *as = (struct aspace *)as_c;                    /* page faults on demand-paged ranges mutate the aspace */
    if (!len) return 1;
    if (va >= USER_TOP || len > USER_TOP || va + len > USER_TOP || va + len < va) return 0;
    for (u64 p = va & ~0xFFFull; p < va + len; p += 4096) {
        u64 *pte = pte_of(as, p, 0);
        if (!pte || !(*pte & P_PRESENT)) {
            if (vm_fault(as, p, write, 0)) return 0;                /* inside a mapping: populate now, so the kernel never faults */
            pte = pte_of(as, p, 0);
        }
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

/* ------------------------------------------------------------ mappings (VMAs) */
struct vma *vma_find(struct aspace *as, u64 addr)
{
    for (int i = 0; i < as->nvma; i++) if (addr >= as->vma[i].start && addr < as->vma[i].end) return &as->vma[i];
    return NULL;
}

int vma_add(struct aspace *as, u64 start, u64 end, int prot, struct inode *ino, u64 off)
{
    if (as->nvma == MAX_VMAS) return -1;
    as->vma[as->nvma++] = (struct vma){ start, end, (u8)prot, ino, off, NULL, 0 };
    return 0;
}

int vma_add_phys(struct aspace *as, u64 start, u64 end, int prot, u64 phys)
{
    if (as->nvma == MAX_VMAS) return -1;
    as->vma[as->nvma++] = (struct vma){ start, end, (u8)prot, NULL, 0, NULL, phys };
    return 0;
}

int vma_add_shm(struct aspace *as, u64 start, u64 end, int prot, u64 off, struct shm *shm)
{
    if (as->nvma == MAX_VMAS) { shm_release(shm); return -1; }
    as->vma[as->nvma++] = (struct vma){ start, end, (u8)prot, NULL, off, shm, 0 };
    return 0;
}

static void vma_remove_range(struct aspace *as, u64 s, u64 e)       /* trims / splits VMAs so nothing overlaps [s,e) */
{
    for (int i = 0; i < as->nvma; i++) {
        struct vma v = as->vma[i];
        if (v.end <= s || v.start >= e) continue;
        if (v.start >= s && v.end <= e) { as->vma[i--] = as->vma[--as->nvma]; if (v.shm) shm_release(v.shm); continue; }
        if (v.start < s && v.end > e) {                                 /* hole in the middle: split */
            as->vma[i].end = s;
            struct vma hi = v; hi.start = e; if (hi.ino || hi.shm) hi.off += e - v.start; if (hi.phys) hi.phys += e - v.start;
            if (as->nvma < MAX_VMAS) { as->vma[as->nvma++] = hi; if (hi.shm) hi.shm->refs++; }
        } else if (v.start < s) as->vma[i].end = s;
        else { if (v.ino || v.shm) as->vma[i].off += e - v.start; if (v.phys) as->vma[i].phys += e - v.start; as->vma[i].start = e; }
    }
}

int vm_munmap(struct aspace *as, u64 start, u64 len)
{
    u64 end = (start + len + 0xFFF) & ~0xFFFull;
    if ((start & 0xFFF) || end <= start || end > USER_TOP) return -1;
    vma_remove_range(as, start, end);
    for (u64 va = start; va < end; va += 4096) {
        u64 *pte = pte_of(as, va, 0);
        if (pte && (*pte & P_PRESENT)) {
            u64 fr = *pte & ADDR_MASK; int shared_pte = (*pte & P_SHARED) != 0; *pte = 0; as->pages--;
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
            if (!shared_pte) pmm_free(fr, 1);
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
        }
    }
    return 0;
}

int vm_mprotect(struct aspace *as, u64 start, u64 len, int prot)
{
    u64 end = (start + len + 0xFFF) & ~0xFFFull;
    if ((start & 0xFFF) || end <= start || end > USER_TOP) return -1;
    for (u64 va = start; va < end; va += 4096) {                     /* no holes allowed */
        u64 *pte = pte_of(as, va, 0);
        if (!vma_find(as, va) && !(pte && (*pte & P_PRESENT))) return -1;
    }
    struct vma tmp[MAX_VMAS]; int nt = 0;
    for (int i = 0; i < as->nvma; i++) {
        struct vma v = as->vma[i];
        if (v.end <= start || v.start >= end) continue;
        u64 s = v.start > start ? v.start : start, e = v.end < end ? v.end : end;
        tmp[nt++] = (struct vma){ s, e, (u8)prot, v.ino, v.off + (v.ino || v.shm ? s - v.start : 0), v.shm, v.phys ? v.phys + (s - v.start) : 0 };
        if (v.shm) v.shm->refs++;                                    /* held across the remove / re-add below */
    }
    vma_remove_range(as, start, end);
    for (int i = 0; i < nt; i++) { if (as->nvma < MAX_VMAS) as->vma[as->nvma++] = tmp[i]; else if (tmp[i].shm) shm_release(tmp[i].shm); as->vma[as->nvma - 1].prot = (u8)prot; }
    for (u64 va = start; va < end; va += 4096) {
        u64 *pte = pte_of(as, va, 0);
        if (!pte || !(*pte & P_PRESENT)) continue;
        u64 f = *pte & ~(P_WRITE | P_NX | P_USER);
        if (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) f |= P_USER;      /* PROT_NONE: frame kept, hidden from ring 3 */
        if (prot & PROT_WRITE) f |= P_WRITE;
        if (!(prot & PROT_EXEC)) f |= P_NX;
        *pte = f;
        __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    }
    return 0;
}

int vm_fault(struct aspace *as, u64 addr, int write, int exec)
{
    addr &= ~0xFFFull;
    struct vma *v = vma_find(as, addr);
    if (!v) return -1;
    if (write && !(v->prot & PROT_WRITE)) return -1;
    if (exec && !(v->prot & PROT_EXEC)) return -1;
    if (!(v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return -1;
    u64 flg = irq_save();                                               /* threads of one process share the table: populate a page exactly once */
    u64 *pte = pte_of(as, addr, 1);
    if (!pte) { irq_restore(flg); return -1; }
    if (*pte & P_PRESENT) { irq_restore(flg); return -1; }             /* present: a protection fault, or another thread just mapped it */
    if (as->pages >= as->limit) { irq_restore(flg); return -1; }
    if (v->phys) {                                                      /* device memory: map the physical page straight in */
        *pte = (v->phys + (addr - v->start)) | P_PRESENT | P_USER | P_SHARED | ((v->prot & PROT_WRITE) ? P_WRITE : 0) | P_NX;
        as->pages++;
        __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
        irq_restore(flg);
        return 0;
    }
    if (v->shm) {                                                       /* shared page: the object owns the frame */
        struct shm *sh = v->shm; u64 idx = (v->off + (addr - v->start)) >> 12;
        if (idx >= sh->npages) { irq_restore(flg); return -1; }
        if (!sh->pg[idx]) {
            u64 nf = pmm_alloc(1);
            if (!nf) { irq_restore(flg); return -1; }
            memset(tbl(nf), 0, 4096);
            if (sh->ino && idx * 4096 < sh->ino->size) inode_read(sh->ino, idx * 4096, tbl(nf), sh->ino->size - idx * 4096 < 4096 ? sh->ino->size - idx * 4096 : 4096);
            sh->pg[idx] = nf;
        }
        *pte = sh->pg[idx] | P_PRESENT | P_USER | P_SHARED | ((v->prot & PROT_WRITE) ? P_WRITE : 0) | ((v->prot & PROT_EXEC) ? 0 : P_NX);
        as->pages++;
        __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
        irq_restore(flg);
        return 0;
    }
    u64 f = pmm_alloc(1);
    if (!f) { irq_restore(flg); return -1; }
    u8 *pg = (u8 *)tbl(f);
    memset(pg, 0, 4096);
    if (v->ino) {
        u64 fo = v->off + (addr - v->start);
        if (fo < v->ino->size) inode_read(v->ino, fo, pg, v->ino->size - fo < 4096 ? v->ino->size - fo : 4096);
    }
    *pte = f | P_PRESENT | P_USER | ((v->prot & PROT_WRITE) ? P_WRITE : 0) | ((v->prot & PROT_EXEC) ? 0 : P_NX);
    as->pages++;
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
    irq_restore(flg);
    return 0;
}

u64 vm_find_gap(struct aspace *as, u64 hint, u64 len)
{
    u64 a = (hint + 0xFFF) & ~0xFFFull;
    for (;;) {
        int clash = 0;
        for (int i = 0; i < as->nvma; i++) if (a < as->vma[i].end && a + len > as->vma[i].start) { a = (as->vma[i].end + 0xFFF) & ~0xFFFull; clash = 1; break; }
        if (!clash) break;
    }
    return a + len < USER_STACK_TOP - (16ull << 20) ? a : 0;
}

static int clone_level(u64 dst_phys, u64 src_phys, int level, struct aspace *dst)
{
    u64 *d = tbl(dst_phys), *s = tbl(src_phys);
    for (int i = 0; i < 512; i++) {
        if (!(s[i] & P_PRESENT)) continue;
        if (level == 1 && (s[i] & P_SHARED)) {                           /* shared mapping: both processes see the same frame */
            if (dst->pages >= dst->limit) return -1;
            d[i] = s[i]; dst->pages++;
        } else if (level == 1) {
            u64 f = pmm_alloc(1);
            if (!f || dst->pages >= dst->limit) { if (f) pmm_free(f, 1); return -1; }
            memcpy(tbl(f), tbl(s[i] & ADDR_MASK), 4096);
            d[i] = f | (s[i] & ~ADDR_MASK);
            dst->pages++;
        } else {
            u64 n = new_table();
            if (!n) return -1;
            d[i] = n | (s[i] & ~ADDR_MASK);
            if (clone_level(n, s[i] & ADDR_MASK, level - 1, dst)) return -1;
        }
    }
    return 0;
}

int vm_clone(struct aspace *dst, const struct aspace *src)
{
    u64 *d = tbl(dst->pml4), *s = tbl(src->pml4);
    for (int i = 0; i < 256; i++) {
        if (!(s[i] & P_PRESENT)) continue;
        u64 n = new_table();
        if (!n) return -1;
        d[i] = n | (s[i] & ~ADDR_MASK);
        if (clone_level(n, s[i] & ADDR_MASK, 3, dst)) return -1;
    }
    dst->nvma = src->nvma;
    memcpy(dst->vma, src->vma, sizeof src->vma);
    for (int i = 0; i < dst->nvma; i++) if (dst->vma[i].shm) dst->vma[i].shm->refs++;
    return 0;
}

int vm_read32_nofault(const struct aspace *as, u64 va, u32 *out)       /* for futex: callable with interrupts off */
{
    u64 *pte = pte_of(as, va & ~3ull, 0);
    if (!pte || !(*pte & P_PRESENT) || !(*pte & P_USER) || (va & 3)) return -1;
    *out = *(u32 *)((u8 *)tbl(*pte & ADDR_MASK) + (va & 0xFFF));
    return 0;
}
