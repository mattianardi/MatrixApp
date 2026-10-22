#include "proc.h"

/* ELF64 loader: ET_EXEC at its own addresses (0x400000...) and ET_DYN (PIE / shared objects / the dynamic linker)
 * with R_X86_64_RELATIVE applied. PT_INTERP is reported so the caller can load the dynamic linker as well. */
struct ehdr { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff; u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } __attribute__((packed));
struct phdr { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } __attribute__((packed));
struct dyn { i64 tag; u64 val; };
struct rela { u64 off; u64 info; i64 addend; };
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PF_X 1
#define PF_W 2
#define PIE_BASE 0x0000555555554000ull

int elf_load(struct aspace *as, const u8 *img, size_t n, u64 forced_bias, struct elf_info *out, const char **err)
{
    memset(out, 0, sizeof *out);
    if (n < sizeof(struct ehdr)) { *err = "file too small"; return -1; }
    const struct ehdr *eh = (const void *)img;
    if (memcmp(eh->ident, "\x7f""ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1) { *err = "not a 64-bit little-endian ELF"; return -1; }
    if (eh->machine != 62) { *err = "not an x86_64 program"; return -1; }
    if (eh->type != 2 && eh->type != 3) { *err = "unsupported ELF type"; return -1; }
    if (eh->phentsize != sizeof(struct phdr) || eh->phnum == 0 || eh->phnum > 48 || eh->phoff + (u64)eh->phnum * sizeof(struct phdr) > n) { *err = "bad program headers"; return -1; }
    u64 bias = eh->type == 3 ? (forced_bias ? forced_bias : PIE_BASE) : 0, top = 0, base0 = ~0ull, dyn_off = 0, dyn_sz = 0, first_off0 = 0;
    int have_first = 0;

    for (int i = 0; i < eh->phnum; i++) {
        const struct phdr *ph = (const void *)(img + eh->phoff + (u64)i * sizeof *ph);
        if (ph->type == PT_DYNAMIC) { dyn_off = ph->offset; dyn_sz = ph->filesz; }
        if (ph->type == PT_INTERP && ph->filesz < sizeof out->interp && ph->offset + ph->filesz <= n) { memcpy(out->interp, img + ph->offset, ph->filesz); out->interp[ph->filesz] = 0; }
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > n || ph->offset + ph->filesz < ph->offset) { *err = "segment outside file"; return -1; }
        u64 va = ph->vaddr + bias, end = va + ph->memsz;
        if (va < 0x1000 || end > USER_STACK_TOP - (64ull << 20) || end < va) { *err = "segment address not allowed"; return -1; }
        u64 pstart = va & ~0xFFFull, pend = (end + 0xFFF) & ~0xFFFull;
        int rc = vm_alloc(as, pstart, (pend - pstart) / 4096, ((ph->flags & PF_W) ? VMF_W : 0) | ((ph->flags & PF_X) ? VMF_X : 0));
        if (rc) { *err = rc == -2 ? "process memory limit exceeded" : "out of memory"; return -1; }
        if (ph->filesz && vm_write(as, va, img + ph->offset, ph->filesz)) { *err = "cannot populate segment"; return -1; }
        if (end > top) top = end;
        if (va < base0) base0 = va;
        if (!have_first && ph->offset == 0) { first_off0 = va; have_first = 1; }
    }
    if (!top) { *err = "no loadable segments"; return -1; }

    if (eh->type == 3 && dyn_off && dyn_off + dyn_sz <= n) {          /* relative relocations; other types are the dynamic linker's job */
        u64 rela = 0, relasz = 0;
        for (const struct dyn *d = (const void *)(img + dyn_off); (const u8 *)(d + 1) <= img + dyn_off + dyn_sz && d->tag; d++) {
            if (d->tag == 7) rela = d->val;
            else if (d->tag == 8) relasz = d->val;
        }
        for (u64 off = 0; off + sizeof(struct rela) <= relasz; off += sizeof(struct rela)) {
            struct rela r; u64 file_off = 0; int found = 0;
            for (int i = 0; i < eh->phnum && !found; i++) {
                const struct phdr *ph = (const void *)(img + eh->phoff + (u64)i * sizeof *ph);
                if (ph->type == PT_LOAD && rela + off >= ph->vaddr && rela + off < ph->vaddr + ph->filesz) { file_off = ph->offset + (rela + off - ph->vaddr); found = 1; }
            }
            if (!found || file_off + sizeof r > n) { *err = "bad relocation table"; return -1; }
            memcpy(&r, img + file_off, sizeof r);
            if ((r.info & 0xFFFFFFFF) != 8) continue;
            u64 val = bias + (u64)r.addend, where = bias + r.off;
            if (vm_write(as, where, &val, 8)) { *err = "relocation target not mapped"; return -1; }
        }
    }
    out->entry = eh->entry + bias;
    out->phdr = first_off0 ? first_off0 + eh->phoff : 0;
    out->phnum = eh->phnum;
    out->base = bias;
    out->top = top;
    return 0;
}
