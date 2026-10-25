#pragma once
#include "kernel.h"

/* Virtual memory. Upper half (PML4[256..511]) = kernel (shared by every address space);
 * lower half = one private user address space per process. */
#define USER_TOP       0x0000800000000000ull
#define USER_STACK_TOP 0x00007FFFFFFFF000ull
#define VMF_W 1
#define VMF_X 2

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAX_VMAS 1536                                /* a Python with numpy, lxml, pydantic... maps several hundred segments */
struct inode;
struct shm;                                                                   /* pages shared between address spaces (MAP_SHARED, kept across fork) */
struct vma { u64 start, end; u8 prot; struct inode *ino; u64 off; struct shm *shm; u64 phys; };   /* ino != NULL: private file mapping; shm != NULL: shared (off = offset into the object); phys != 0: device memory (frame buffer) */
struct aspace { u64 pml4; size_t pages, limit; int nvma; struct vma vma[MAX_VMAS]; };

void vm_init(void);
void vm_tunables(void);                          /* drops the boot identity map, enables SMEP/SMAP when present */
u64  vm_kernel_pml4(void);
int  vm_new(struct aspace *as, size_t page_limit);
void vm_free(struct aspace *as);              /* releases every user page and the page tables */
void vm_switch(u64 pml4);
int  vm_alloc(struct aspace *as, u64 va, size_t npages, int vmf);   /* zeroed pages; existing pages keep data, gain permissions */
int  vm_write(struct aspace *as, u64 va, const void *src, size_t n); /* kernel -> user memory, bypasses user permissions */
int  user_range_ok(const struct aspace *as, u64 va, size_t len, int write);
int  copy_from_user(const struct aspace *as, void *dst, u64 usrc, size_t n);
int  copy_to_user(const struct aspace *as, u64 udst, const void *src, size_t n);
int  strncpy_from_user(const struct aspace *as, char *dst, u64 usrc, size_t max);   /* length, or -1 */
int  vma_add(struct aspace *as, u64 start, u64 end, int prot, struct inode *ino, u64 off);
struct vma *vma_find(struct aspace *as, u64 addr);
struct shm *shm_get(struct inode *ino, u64 npages);          /* shared object for a file (one per inode) or a fresh anonymous one; takes a reference */
int  vma_add_shm(struct aspace *as, u64 start, u64 end, int prot, u64 off, struct shm *shm);   /* consumes the reference from shm_get */
int  vma_add_phys(struct aspace *as, u64 start, u64 end, int prot, u64 phys);   /* map device memory (never freed with the address space) */
void shm_sync(struct shm *s);                                /* write file-backed shared pages back to the file (msync) */
u64  shm_key(const struct vma *v, u64 addr);                 /* identity of a shared word (futex key) */
int  vm_fault(struct aspace *as, u64 addr, int write, int exec);    /* demand-page one page; 0 = handled */
int  vm_munmap(struct aspace *as, u64 start, u64 len);
int  vm_mprotect(struct aspace *as, u64 start, u64 len, int prot);   /* 0 / -1 (hole in the range) */
int  vm_clone(struct aspace *dst, const struct aspace *src);         /* eager copy (fork) */
u64  vm_find_gap(struct aspace *as, u64 hint, u64 len);
int  vm_read32_nofault(const struct aspace *as, u64 va, u32 *out);
int  vm_smep(void);
int  vm_smap(void);

/* gdt.c */
void gdt_init(void);
void syscall_init(void);
void linux_init(void);
void tss_set_rsp0(u64 rsp0);
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x1B
#define SEL_UCODE 0x23
