#pragma once
#include "kernel.h"

/* Virtual memory. Upper half (PML4[256..511]) = kernel (shared by every address space);
 * lower half = one private user address space per process. */
#define USER_TOP       0x0000800000000000ull
#define USER_STACK_TOP 0x00007FFFFFFFF000ull
#define VMF_W 1
#define VMF_X 2

struct aspace { u64 pml4; size_t pages, limit; };

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
int  vm_smep(void);
int  vm_smap(void);

/* gdt.c */
void gdt_init(void);
void tss_set_rsp0(u64 rsp0);
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x1B
#define SEL_UCODE 0x23
