#include "proc.h"
#include "sock.h"
#include "serial.h"
#include "chr.h"
#include "fs.h"
#include "agent.h"
#include "neos_api.h"
#include "vfs.h"
#include "varbus.h"

/*
 * Linux x86-64 system call interface (the `syscall` instruction). Unmodified static - and, with a dynamic linker in
 * the filesystem, dynamic - Linux programs run on top of the NEOS kernel objects: inodes, pipes, address spaces.
 * Unimplemented calls return -ENOSYS and are reported once (linux.trace) so gaps are visible, not silent.
 */
#define AT_FDCWD (-100)
#define EXTRA_ENOSYS 38
#define E_2BIG 7
#define E_NOEXEC 8
#define E_TIMEDOUT 110
#define E_NODEV 19

static struct tunable *t_trace, *t_release, *t_sysname;
static u8 reported[512];

/* ------------------------------------------------------------ helpers */
static long get_path(struct proc *p, u64 uptr, char *abs, size_t n)
{
    char raw[256];
    int l = strncpy_from_user(&p->as, raw, uptr, sizeof raw);
    if (l < 0) return -E_FAULT;
    if (!l) return -E_NOENT;
    fs_norm_at(p->cwd[0] ? p->cwd : "/", raw, abs, n);
    return 0;
}

static long path_at(struct proc *p, long dirfd, u64 uptr, char *abs, size_t n)
{
    char raw[256];
    int l = strncpy_from_user(&p->as, raw, uptr, sizeof raw);
    if (l < 0) return -E_FAULT;
    dirfd = (long)(int)dirfd;                                          /* the ABI passes a 32-bit int: -100 (AT_FDCWD) arrives zero-extended */
    if (raw[0] != '/' && dirfd != AT_FDCWD) {
        struct file *d = fdt_get(p->fds, (int)dirfd);
        if (!d) return -E_BADF;
        if (d->type != FT_DIR) return -E_NOTDIR;
        fs_norm_at(d->ino->path, raw, abs, n);
        return 0;
    }
    if (!l) return -E_NOENT;
    fs_norm_at(p->cwd[0] ? p->cwd : "/", raw, abs, n);
    return 0;
}

static struct inode *follow(struct inode *i, int depth)
{
    while (i && (i->mode & S_IFMT) == S_IFLNK && depth++ < 8) {
        char t[128];
        char dir[128]; strlcpy(dir, i->path, sizeof dir);
        char *sl = dir + strlen(dir); while (sl > dir && *sl != '/') sl--; *sl = 0;
        fs_norm_at(dir[0] ? dir : "/", i->link, t, sizeof t);
        i = vfs_lookup(t);
    }
    return i;
}

static void put_stat(u8 *st, u32 mode, u64 ino, u64 size, u64 mtime, u64 rdev)
{
    memset(st, 0, 144);
    *(u64 *)(st + 0) = 1; *(u64 *)(st + 8) = ino; *(u64 *)(st + 16) = 1; *(u32 *)(st + 24) = mode;
    *(u64 *)(st + 40) = rdev; *(i64 *)(st + 48) = (i64)size; *(i64 *)(st + 56) = 4096; *(i64 *)(st + 64) = (i64)((size + 511) / 512);
    *(i64 *)(st + 72) = (i64)mtime; *(i64 *)(st + 88) = (i64)mtime; *(i64 *)(st + 104) = (i64)mtime;
}

static long stat_inode(struct proc *p, struct inode *i, u64 ust)
{
    u8 st[144];
    put_stat(st, i->mode, i->ino, i->size, i->mtime, i->dev == DEV_TTY ? 0x500 : i->dev ? 0x100 + (u64)i->dev : 0);
    return copy_to_user(&p->as, ust, st, sizeof st) ? -E_FAULT : 0;
}

static long stat_file(struct proc *p, struct file *f, u64 ust)
{
    u8 st[144];
    switch (f->type) {
    case FT_INODE: case FT_DIR: return stat_inode(p, f->ino, ust);
    case FT_TTY: put_stat(st, S_IFCHR | 0620, 3, 0, vfs_now(), 0x500); break;
    case FT_SERIAL: put_stat(st, S_IFCHR | 0666, 4, 0, vfs_now(), 0x400 + (u64)f->serial); break;
    case FT_CHR: put_stat(st, S_IFCHR | 0666, 5, 0, vfs_now(), 0x1300 + (u64)f->chr); break;
    case FT_SOCK: put_stat(st, 0140000 | 0777, 5, 0, vfs_now(), 0); break;
    default: put_stat(st, S_IFIFO | 0600, 4, 0, vfs_now(), 0); break;
    }
    return copy_to_user(&p->as, ust, st, sizeof st) ? -E_FAULT : 0;
}

/* ------------------------------------------------------------ files */
static long do_open(struct proc *p, char *abs, int flags, u32 mode)
{
    struct inode *i = vfs_lookup(abs);
    if (!i) {
        if (!(flags & O_CREAT)) return -E_NOENT;
        int r = vfs_create(abs, S_IFREG | (mode & 0777 & ~022u), &i);
        if (r) return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) return -E_EXIST;
    if (!(flags & O_NOFOLLOW)) { i = follow(i, 0); if (!i) return -E_NOENT; }
    int dir = (i->mode & S_IFMT) == S_IFDIR;
    if (dir && (flags & O_ACCMODE)) return -E_ISDIR;
    if ((flags & O_DIRECTORY) && !dir) return -E_NOTDIR;
    int ser = i->dev >= DEV_SERIAL0 && i->dev < DEV_SERIAL0 + SER_MAX, chr = i->dev >= DEV_CHR0 && i->dev < DEV_CHR0 + CHR_MAX;
    if (chr) { const struct chrops *o = chr_ops(i->dev - DEV_CHR0); if (!o) return -E_NODEV; if (o->open && o->open(chr_minor(i->dev - DEV_CHR0)) < 0) return -E_NODEV; }
    struct file *f = file_new(dir ? FT_DIR : i->dev == DEV_TTY ? FT_TTY : ser ? FT_SERIAL : chr ? FT_CHR : FT_INODE, flags);
    if (!f) return -E_NOMEM;
    f->ino = i; inode_ref(i); if (ser) f->serial = i->dev - DEV_SERIAL0; if (chr) f->chr = i->dev - DEV_CHR0;
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) && !dir && !i->dev) inode_truncate(i, 0);
    int fd = fdt_install(p->fds, f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long rw_user(struct proc *p, struct file *f, u64 ubuf, u64 len, int write)
{
    if (!len) return 0;
    size_t chunk = len < 65536 ? len : 65536;
    u8 *b = kmalloc(chunk);
    if (!b) return -E_NOMEM;
    long total = 0;
    while ((u64)total < len) {
        size_t c = len - (u64)total < chunk ? len - (u64)total : chunk;
        long r;
        if (write) {
            if (copy_from_user(&p->as, b, ubuf + (u64)total, c)) { r = total ? total : -E_FAULT; kfree(b); return r; }
            r = file_write(f, b, c);
        } else {
            r = file_read(f, b, c);
            if (r > 0 && copy_to_user(&p->as, ubuf + (u64)total, b, (size_t)r)) { kfree(b); return total ? total : -E_FAULT; }
        }
        if (r < 0) { kfree(b); return total ? total : r; }
        total += r;
        if ((size_t)r < c || f->type == FT_TTY || f->type == FT_SERIAL || f->type == FT_CHR || f->type == FT_PIPE_R || f->type == FT_SOCK || f->type == FT_EVENTFD) break;      /* short read / interactive: return what we have */
    }
    kfree(b);
    return total;
}

static long sys_readv(struct proc *p, int fd, u64 uiov, long cnt, int write)
{
    struct file *f = fdt_get(p->fds, fd);
    if (!f) return -E_BADF;
    if (cnt < 0 || cnt > 64) return -E_INVAL;
    long total = 0;
    for (long i = 0; i < cnt; i++) {
        u64 iov[2];
        if (copy_from_user(&p->as, iov, uiov + (u64)i * 16, 16)) return total ? total : -E_FAULT;
        long r = rw_user(p, f, iov[0], iov[1], write);
        if (r < 0) return total ? total : r;
        total += r;
        if ((u64)r < iov[1]) break;
    }
    return total;
}

static long sys_getdents(struct proc *p, int fd, u64 ubuf, u64 count)
{
    struct file *f = fdt_get(p->fds, fd);
    if (!f) return -E_BADF;
    if (f->type != FT_DIR) return -E_NOTDIR;
    u8 *out = kmalloc(count < 65536 ? count : 65536);
    if (!out) return -E_NOMEM;
    size_t used = 0, cap = count < 65536 ? count : 65536;
    for (;;) {
        char name[256]; int type; u32 ino;
        i64 idx = f->off;
        if (idx == 0) { strlcpy(name, ".", sizeof name); type = S_IFDIR; ino = f->ino->ino; }
        else if (idx == 1) { strlcpy(name, "..", sizeof name); type = S_IFDIR; ino = 1; }
        else if (vfs_readdir(f->ino->path, (int)idx - 2, name, sizeof name, &type, &ino)) break;
        size_t nl = strlen(name), reclen = (19 + nl + 1 + 7) & ~7ul;
        if (used + reclen > cap) { if (!used) { kfree(out); return -E_INVAL; } break; }
        u8 *d = out + used;
        u8 dt = type == S_IFDIR ? 4 : type == S_IFLNK ? 10 : type == S_IFCHR ? 2 : 8;
        *(u64 *)d = ino; *(i64 *)(d + 8) = idx + 1; *(u16 *)(d + 16) = (u16)reclen; d[18] = dt;
        memcpy(d + 19, name, nl + 1);
        used += reclen; f->off++;
    }
    long rc = used ? (copy_to_user(&p->as, ubuf, out, used) ? -E_FAULT : (long)used) : 0;
    kfree(out);
    return rc;
}

/* ------------------------------------------------------------ memory */
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANON 0x20

static long sys_mmap(struct proc *p, u64 addr, u64 len, int prot, int flags, long fd, u64 off)
{
    u64 plen = (len + 0xFFF) & ~0xFFFull;
    if (!len || plen < len) return -E_INVAL;
    struct inode *ino = NULL;
    if (!(flags & MAP_ANON)) {
        struct file *f = fdt_get(p->fds, (int)fd);
        if (!f) return -E_BADF;
        if (f->type == FT_CHR) {                                         /* device memory (/dev/fb0) */
            const struct chrops *o = chr_ops(f->chr);
            if (!o->mmap) return -E_NODEV;
            u64 va;
            if (flags & MAP_FIXED) { if ((addr & 0xFFF) || addr < 0x1000 || addr + plen > USER_TOP) return -E_INVAL; vm_munmap(&p->as, addr, plen); va = addr; }
            else { va = vm_find_gap(&p->as, addr && addr >= 0x10000 ? addr : p->mmap_next, plen); if (!va) return -E_NOMEM; if (!addr) p->mmap_next = va + plen; }
            int e = o->mmap(chr_minor(f->chr), p, va, plen, prot, off);
            return e ? e : (long)va;
        }
        if (f->type != FT_INODE || (f->ino->mode & S_IFMT) != S_IFREG || f->ino->dev) return -E_NODEV;
        ino = f->ino;
    }
    u64 va;
    if (flags & MAP_FIXED) {
        if ((addr & 0xFFF) || addr < 0x1000 || addr + plen > USER_TOP) return -E_INVAL;
        vm_munmap(&p->as, addr, plen);
        va = addr;
    } else {
        va = vm_find_gap(&p->as, addr && addr >= 0x10000 ? addr : p->mmap_next, plen);
        if (!va) return -E_NOMEM;
        if (!addr) p->mmap_next = va + plen;
    }
    if (flags & MAP_SHARED) {                                           /* shared with every other mapper of the file (or anonymous, inherited by fork) */
        struct shm *sh = shm_get(ino, ((ino ? off : 0) + plen) >> 12);
        if (!sh) return -E_NOMEM;
        if (vma_add_shm(&p->as, va, va + plen, prot, ino ? off : 0, sh)) return -E_NOMEM;
        return (long)va;
    }
    if (vma_add(&p->as, va, va + plen, prot, ino, off)) return -E_NOMEM;
    return (long)va;
}

static long sys_brk(struct proc *p, u64 nb)
{
    if (!nb || nb < p->brk_start) return (long)p->brk;
    u64 oldp = (p->brk + 0xFFF) & ~0xFFFull, newp = (nb + 0xFFF) & ~0xFFFull;
    if (nb > p->brk_start + (1ull << 36)) return (long)p->brk;
    struct vma *h = NULL;
    for (int i = 0; i < p->as.nvma; i++) if (p->as.vma[i].start == p->brk_start) h = &p->as.vma[i];
    if (newp > oldp) {
        for (int i = 0; i < p->as.nvma; i++) if (&p->as.vma[i] != h && p->as.vma[i].start < newp && p->as.vma[i].end > oldp) return (long)p->brk;
        if (h) h->end = newp; else if (newp > p->brk_start && vma_add(&p->as, p->brk_start, newp, PROT_READ | PROT_WRITE, NULL, 0)) return (long)p->brk;
    } else if (newp < oldp) {
        vm_munmap(&p->as, newp, oldp - newp);
        for (int i = 0; i < p->as.nvma; i++) if (p->as.vma[i].start == p->brk_start) { if (newp <= p->brk_start) p->as.vma[i] = p->as.vma[--p->as.nvma]; else p->as.vma[i].end = newp; break; }
    }
    p->brk = nb;
    return (long)nb;
}

/* ------------------------------------------------------------ futex */
/* A futex is identified by (address space, address) - or (shared object, offset) for shared mappings. The key is a 63-bit hash used only as an
 * opaque wait-queue identity (kernel objects live in the upper half, so it never clashes with them). With a small bucket table two different
 * futexes sharing a bucket made wake(1) pick the wrong sleeper and lose the wake-up: the server hung on its second request. */
static void *fkey(struct proc *p, u64 addr)
{
    struct vma *v = vma_find(&p->as, addr);
    u64 h = (v && v->shm) ? shm_key(v, addr) * 0x9E3779B97F4A7C15ull : ((p->as.pml4 >> 12) * 0x9E3779B97F4A7C15ull) ^ ((addr >> 2) * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return (void *)((h & 0x7FFFFFFFFFFFFFFFull) | 0x1000);
}

void futex_wake_addr(struct proc *p, u64 addr, int n) { wake_on(fkey(p, addr), n); }

static u64 ts_to_deadline(struct proc *p, u64 uts, int absolute)
{
    u64 ts[2];
    if (copy_from_user(&p->as, ts, uts, 16)) return 0;
    u64 ns = ts[0] * 1000000000ull + ts[1];
    if (absolute) { u64 now = time_ns(); ns = ns > now ? ns - now : 0; }
    return ticks + ms_to_ticks((ns + 999999) / 1000000);
}

static long sys_futex(struct proc *p, u64 uaddr, int op, u32 val, u64 utime, u64 uaddr2, u32 val3)
{
    int cmd = op & 127;
    (void)val3;
    switch (cmd) {
    case 0: case 9: {                                              /* WAIT, WAIT_BITSET */
        if (!user_range_ok(&p->as, uaddr, 4, 0)) return -E_FAULT;
        u64 deadline = ~0ull;
        if (utime) { deadline = ts_to_deadline(p, utime, cmd == 9); if (!deadline) return -E_FAULT; }
        void *key = fkey(p, uaddr);
        u64 fl = irq_save();
        u32 cur;
        if (vm_read32_nofault(&p->as, uaddr, &cur)) { irq_restore(fl); return -E_FAULT; }
        if (cur != val) { irq_restore(fl); return -E_AGAIN; }
        sleep_on_locked(key, deadline);
        irq_restore(fl);
        return (deadline != ~0ull && ticks >= deadline) ? -E_TIMEDOUT : 0;
    }
    case 1: case 10: return wake_on(fkey(p, uaddr), (int)val);
    case 3: case 4: {                                              /* REQUEUE, CMP_REQUEUE */
        if (cmd == 4) { u32 cur; if (copy_from_user(&p->as, &cur, uaddr, 4)) return -E_FAULT; if (cur != val3) return -E_AGAIN; }
        int woken = wake_on(fkey(p, uaddr), (int)val);
        extern int requeue_on(void *from, void *to, int max);
        return woken + requeue_on(fkey(p, uaddr), fkey(p, uaddr2), (int)utime);
    }
    case 5: {                                                      /* WAKE_OP */
        u32 enc = val3, v2; int op = (enc >> 28) & 7, cmp = (enc >> 24) & 15; u32 oa = (enc >> 12) & 0xFFF, ca = enc & 0xFFF;
        if (enc & 0x80000000u) oa = 1u << oa;
        if (copy_from_user(&p->as, &v2, uaddr2, 4)) return -E_FAULT;
        u32 nv = op == 0 ? oa : op == 1 ? v2 + oa : op == 2 ? v2 | oa : op == 3 ? v2 & ~oa : v2 ^ oa;
        if (copy_to_user(&p->as, uaddr2, &nv, 4)) return -E_FAULT;
        int woken = wake_on(fkey(p, uaddr), (int)val);
        int hit = cmp == 0 ? v2 == ca : cmp == 1 ? v2 != ca : cmp == 2 ? v2 < ca : cmp == 3 ? v2 <= ca : cmp == 4 ? v2 > ca : v2 >= ca;
        if (hit) woken += wake_on(fkey(p, uaddr2), (int)utime);
        return woken;
    }
    case 6: case 7: case 8: {                                      /* LOCK_PI / UNLOCK_PI / TRYLOCK_PI (owner = user thread id; no priority boosting yet) */
        if (!user_range_ok(&p->as, uaddr, 4, 1)) return -E_FAULT;
        u32 tid = (u32)uctx_current()->utid; void *key = fkey(p, uaddr);
        u64 deadline = ~0ull;
        if (cmd == 7) {
            u64 fl = irq_save(); u32 cur;
            if (vm_read32_nofault(&p->as, uaddr, &cur)) { irq_restore(fl); return -E_FAULT; }
            if ((cur & 0x3FFFFFFF) != tid) { irq_restore(fl); return -1; }       /* EPERM */
            u32 z = 0; vm_write(&p->as, uaddr, &z, 4); irq_restore(fl);
            wake_on(key, 1); return 0;
        }
        if (cmd == 6 && utime) { deadline = ts_to_deadline(p, utime, 1); if (!deadline) return -E_FAULT; }
        int waited = 0;
        for (;;) {
            u64 fl = irq_save(); u32 cur;
            if (vm_read32_nofault(&p->as, uaddr, &cur)) { irq_restore(fl); return -E_FAULT; }
            if (cur == 0 || ((cur & 0x3FFFFFFF) == 0)) { u32 nv = tid | (waited ? 0x80000000u : 0); vm_write(&p->as, uaddr, &nv, 4); irq_restore(fl); return 0; }
            if ((cur & 0x3FFFFFFF) == tid) { irq_restore(fl); return -35; }      /* EDEADLK */
            if (cmd == 8) { irq_restore(fl); return -E_AGAIN; }
            u32 nv = cur | 0x80000000u; vm_write(&p->as, uaddr, &nv, 4);
            waited = 1;
            sleep_on_locked(key, deadline);
            irq_restore(fl);
            if (deadline != ~0ull && ticks >= deadline) return -E_TIMEDOUT;
            if (sig_interrupted(p)) return -E_INTR;
        }
    }
    }
    return -E_NOSYS;
}

/* ------------------------------------------------------------ misc */
static long sys_nanosleep(struct proc *p, u64 ureq, u64 urem)
{
    u64 ts[2];
    if (copy_from_user(&p->as, ts, ureq, 16)) return -E_FAULT;
    u64 ns = ts[0] * 1000000000ull + ts[1];
    u64 ms = (ns + 999999) / 1000000, until = ticks + ms_to_ticks(ms);
    while (ticks < until) {
        if (sig_interrupted(p)) { u64 left = (until - ticks) * 1000000000ull / timer_hz(); if (urem) { u64 r[2] = { left / 1000000000ull, left % 1000000000ull }; copy_to_user(&p->as, urem, r, 16); } return -E_INTR; }
        thread_sleep_ms(ms > 4 ? 4 : ms ? ms : 1);
    }
    if (urem) { u64 z[2] = { 0, 0 }; copy_to_user(&p->as, urem, z, 16); }
    return 0;
}

static long sys_clock_gettime(struct proc *p, int clk, u64 uts)
{
    u64 ns = (clk == 0 || clk == 5) ? wall_ns() : time_ns();
    u64 ts[2] = { ns / 1000000000ull, ns % 1000000000ull };
    return copy_to_user(&p->as, uts, ts, 16) ? -E_FAULT : 0;
}

static long sys_uname(struct proc *p, u64 ub)
{
    char u[6][65];
    memset(u, 0, sizeof u);
    strlcpy(u[0], t_sysname->sval, 65); strlcpy(u[1], "neos", 65); strlcpy(u[2], t_release->sval, 65);
    strlcpy(u[3], "#1 NEOS " NEOS_VERSION, 65); strlcpy(u[4], "x86_64", 65); strlcpy(u[5], "(none)", 65);
    return copy_to_user(&p->as, ub, u, sizeof u) ? -E_FAULT : 0;
}

static long sys_poll(struct proc *p, u64 ufds, u64 nfds, long timeout_ms)
{
    if (nfds > 64) return -E_INVAL;
    u64 deadline = timeout_ms < 0 ? ~0ull : ticks + ms_to_ticks((u64)timeout_ms);
    for (;;) {
        int ready = 0;
        for (u64 i = 0; i < nfds; i++) {
            struct { int fd; short events, revents; } pf;
            if (copy_from_user(&p->as, &pf, ufds + i * 8, 8)) return -E_FAULT;
            pf.revents = 0;
            struct file *f = pf.fd >= 0 ? fdt_get(p->fds, pf.fd) : NULL;
            if (pf.fd >= 0 && !f) pf.revents = 0x20;                         /* POLLNVAL */
            else if (f) {
                pf.revents = (short)file_poll_ready(f, pf.events);
                if (f->type == FT_PIPE_R && !f->pipe) pf.revents |= 0x10;
            }
            if (pf.revents) ready++;
            copy_to_user(&p->as, ufds + i * 8, &pf, 8);
        }
        if (ready || timeout_ms == 0 || (deadline != ~0ull && ticks >= deadline)) return ready;
        if (sig_interrupted(p)) return -E_INTR;
        extern int tty_pump(void);
        tty_pump();
        thread_sleep_ms(2);
    }
}

static long sys_select(struct proc *p, int nfds, u64 ur, u64 uw, u64 ue, u64 utmo, int is_timespec)
{
    if (nfds < 0 || nfds > 1024) return -E_INVAL;
    if (nfds > MAX_FDS) nfds = MAX_FDS;
    size_t nb = ((size_t)nfds + 7) / 8;
    u8 in[3][16], out[3][16]; u64 uptr[3] = { ur, uw, ue };
    memset(in, 0, sizeof in);
    for (int k = 0; k < 3; k++) if (uptr[k] && nb && copy_from_user(&p->as, in[k], uptr[k], nb)) return -E_FAULT;
    long timeout_ms = -1;
    if (utmo) {
        u64 ts[2]; if (copy_from_user(&p->as, ts, utmo, 16)) return -E_FAULT;
        timeout_ms = (long)(ts[0] * 1000 + ts[1] / (is_timespec ? 1000000 : 1000));
    }
    u64 deadline = timeout_ms < 0 ? ~0ull : ticks + ms_to_ticks((u64)timeout_ms);
    static const int ev[3] = { 1, 4, 0 };
    for (;;) {
        int ready = 0; memset(out, 0, sizeof out);
        for (int fd = 0; fd < nfds; fd++) for (int k = 0; k < 2; k++) {
            if (!(in[k][fd / 8] & (1 << (fd % 8)))) continue;
            struct file *f = fdt_get(p->fds, fd);
            if (!f) return -E_BADF;
            if (file_poll_ready(f, ev[k]) & (ev[k] | 0x18)) { out[k][fd / 8] |= (u8)(1 << (fd % 8)); ready++; }
        }
        if (ready || timeout_ms == 0 || (deadline != ~0ull && ticks >= deadline)) {
            for (int k = 0; k < 3; k++) if (uptr[k] && nb && copy_to_user(&p->as, uptr[k], out[k], nb)) return -E_FAULT;
            return ready;
        }
        if (sig_interrupted(p)) return -E_INTR;
        extern int tty_pump(void);
        tty_pump();
        thread_sleep_ms(1);
    }
}

static long sys_ioctl(struct proc *p, int fd, u64 req, u64 arg)
{
    struct file *f = fdt_get(p->fds, fd);
    if (!f) return -E_BADF;
    if (f->type == FT_SOCK) {
        if (req == 0x5421) { int on = 0; if (copy_from_user(&p->as, &on, arg, 4)) return -E_FAULT; if (on) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK; return 0; }
        if (req == 0x541B) { int n = sock_nread(f->sock); return copy_to_user(&p->as, arg, &n, 4) ? -E_FAULT : 0; }
        return -E_NOTTY;
    }
    if (f->type == FT_SERIAL) return serial_ioctl(f->serial, req, arg, p);
    if (f->type == FT_CHR) { const struct chrops *o = chr_ops(f->chr); return o->ioctl ? o->ioctl(chr_minor(f->chr), req, arg, p) : -E_NOTTY; }
    if (f->type != FT_TTY) return -E_NOTTY;
    switch (req) {
    case 0x5401: {                                                         /* TCGETS: a cooked, echoing terminal */
        u8 t[36]; memset(t, 0, sizeof t);
        *(u32 *)t = 0x500; *(u32 *)(t + 4) = 5; *(u32 *)(t + 8) = 0xBF; *(u32 *)(t + 12) = 0x8A3B; t[17 + 0] = 3; t[17 + 4] = 4;
        return copy_to_user(&p->as, arg, t, sizeof t) ? -E_FAULT : 0;
    }
    case 0x5402: case 0x5403: case 0x5404: return 0;                       /* TCSETS*: accepted (line discipline stays cooked) */
    case 0x540F: { int pg = p->pid; return copy_to_user(&p->as, arg, &pg, 4) ? -E_FAULT : 0; }
    case 0x5410: return 0;
    case 0x5413: { u16 ws[4] = { 25, 80, 0, 0 }; return copy_to_user(&p->as, arg, ws, 8) ? -E_FAULT : 0; }
    case 0x541B: { int n = tty_has_input() ? 1 : 0; return copy_to_user(&p->as, arg, &n, 4) ? -E_FAULT : 0; }
    }
    return -E_NOTTY;
}

static long copy_vec(struct proc *p, u64 uv, struct argblock *ab, int env)
{
    if (!uv) return 0;
    char tmp[4096];
    for (int i = 0; i < 256; i++) {
        u64 sp;
        if (copy_from_user(&p->as, &sp, uv + (u64)i * 8, 8)) return -E_FAULT;
        if (!sp) return 0;
        int l = strncpy_from_user(&p->as, tmp, sp, sizeof tmp);
        if (l < 0) return -E_FAULT;
        if (argblock_add(ab, env, tmp)) return -E_2BIG;
    }
    return -E_2BIG;
}

static int proc_hook(const char *path, char *buf, size_t n)
{
    struct proc *p = proc_current();
    if (!strcmp(path, "/proc/self/exe")) return -1;
    if (!strcmp(path, "/proc/version")) return ksnprintf(buf, n, "%s version %s (neos) #1 NEOS %s\n", t_sysname->sval, t_release->sval, NEOS_VERSION);
    if (!strcmp(path, "/proc/uptime")) { u64 ms = time_ns() / 1000000; return ksnprintf(buf, n, "%lu.%02lu 0.00\n", ms / 1000, ms / 10 % 100); }
    if (!strcmp(path, "/proc/meminfo"))
        return ksnprintf(buf, n, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\nBuffers:               0 kB\nCached:                0 kB\n",
                         (u64)pmm_total_pages() * 4, (u64)pmm_free_pages() * 4, (u64)pmm_free_pages() * 4);
    if (!strcmp(path, "/proc/cpuinfo")) return ksnprintf(buf, n, "processor\t: 0\nvendor_id\t: NEOS\nmodel name\t: %s\ncpu MHz\t\t: %lu\nflags\t\t: fpu sse sse2 syscall nx lm\n", cpu_brand(), tsc_hz() / 1000000);
    if (!strcmp(path, "/proc/self/maps") && p) {
        size_t l = 0;
        for (int i = 0; i < p->as.nvma && l + 80 < n; i++) {
            struct vma *v = &p->as.vma[i];
            l += ksnprintf(buf + l, n - l, "%012lx-%012lx %c%c%cp 00000000 00:00 0\n", v->start, v->end, v->prot & 1 ? 'r' : '-', v->prot & 2 ? 'w' : '-', v->prot & 4 ? 'x' : '-');
        }
        return (int)l;
    }
    if (!strcmp(path, "/proc/self/stat") && p) return ksnprintf(buf, n, "%d (%s) R %d 0 0 0 0 0 0 0 0 0 0 0 0 0 20 0 %d 0 0 0 0\n", p->pid, p->name, p->ppid, p->nthreads);
    return -1;
}

/* ------------------------------------------------------------ sockets, epoll, eventfd (IPv4 TCP/UDP over the kernel stack) */
#define SOCK_NONBLOCK_ 04000
#define SOCK_CLOEXEC_ 02000000
#define MSG_DONTWAIT_ 0x40

static struct sock *fd_sock(struct proc *p, int fd, struct file **fo)
{
    struct file *f = fdt_get(p->fds, fd);
    if (!f || f->type != FT_SOCK) return NULL;
    if (fo) *fo = f;
    return f->sock;
}

/* AF_UNIX is carried over IPv4 loopback: a path (or abstract name) maps to a private loopback port, and bind() leaves an empty file behind */
#define UNIX_MAX 64
static struct { char path[110]; u16 port; } unix_tab[UNIX_MAX];
static u16 unix_next = 20000;
static int unix_name(struct proc *p, u64 ua, u64 len, char *out, int abstract_ok)
{
    char raw[110]; memset(raw, 0, sizeof raw);
    if (len <= 2) return -E_INVAL;
    u64 n = len - 2 < 108 ? len - 2 : 108;
    if (copy_from_user(&p->as, raw, ua + 2, n)) return -E_FAULT;
    if (!raw[0]) {                                                      /* abstract: "@name" */
        if (!abstract_ok || n < 2) return -E_INVAL;
        out[0] = '@'; memcpy(out + 1, raw + 1, n - 1); out[n] = 0; return 1;
    }
    char abs[256]; long r = get_path(p, ua + 2, abs, sizeof abs); if (r) return (int)r;
    strlcpy(out, abs, 110); return 0;
}
static long unix_lookup(struct proc *p, u64 ua, u64 len, u16 *port)
{
    char nm[110]; int r = unix_name(p, ua, len, nm, 1); if (r < 0) return r;
    for (int i = 0; i < UNIX_MAX; i++) if (unix_tab[i].port && !strcmp(unix_tab[i].path, nm)) {
        if (nm[0] != '@' && !vfs_lookup(nm)) { unix_tab[i].port = 0; break; }          /* the socket file was removed */
        *port = unix_tab[i].port; return 0;
    }
    return nm[0] == '@' ? -111 : -2;                                    /* ECONNREFUSED / ENOENT */
}
static long unix_bind(struct proc *p, struct sock *sk, u64 ua, u64 len)
{
    char nm[110]; int r = unix_name(p, ua, len, nm, 1); if (r < 0) return r;
    int slot = -1;
    for (int i = 0; i < UNIX_MAX; i++) {
        if (unix_tab[i].port && !strcmp(unix_tab[i].path, nm)) { if (nm[0] == '@' || vfs_lookup(nm)) return -98; slot = i; break; }
        if (!unix_tab[i].port && slot < 0) slot = i;
    }
    if (slot < 0) return -E_NOMEM;
    u16 port = unix_next++; if (unix_next >= 30000) unix_next = 20000;
    long e = sock_bind(sk, 0x7F000001, port); if (e) return e;
    if (nm[0] != '@' && fs_write(nm, "", 0)) return -E_NOMEM;
    strlcpy(unix_tab[slot].path, nm, sizeof unix_tab[slot].path); unix_tab[slot].port = port;
    return 0;
}
static long get_sockaddr(struct proc *p, u64 ua, u64 len, u32 *ip, u16 *port)
{
    u8 sa[16];
    if (len < 2) return -E_INVAL;
    if (copy_from_user(&p->as, sa, ua, 2)) return -E_FAULT;
    if (*(u16 *)sa == 1) { *ip = 0x7F000001; return unix_lookup(p, ua, len, port); }
    if (len < 8) return -E_INVAL;
    if (copy_from_user(&p->as, sa, ua, 16 < len ? 16 : len)) return -E_FAULT;
    if (*(u16 *)sa != 2) return -97;                                    /* EAFNOSUPPORT: IPv4 and AF_UNIX only */
    *port = (u16)(sa[2] << 8 | sa[3]); *ip = (u32)sa[4] << 24 | (u32)sa[5] << 16 | (u32)sa[6] << 8 | sa[7];
    return 0;
}
static long put_sockaddr(struct proc *p, u64 ua, u64 ulen, u32 ip, u16 port)
{
    if (!ua || !ulen) return 0;
    u8 sa[16]; memset(sa, 0, 16);
    *(u16 *)sa = 2; sa[2] = (u8)(port >> 8); sa[3] = (u8)port; sa[4] = (u8)(ip >> 24); sa[5] = (u8)(ip >> 16); sa[6] = (u8)(ip >> 8); sa[7] = (u8)ip;
    u32 have = 0; if (copy_from_user(&p->as, &have, ulen, 4)) return -E_FAULT;
    if (copy_to_user(&p->as, ua, sa, have < 16 ? have : 16)) return -E_FAULT;
    u32 n = 16; return copy_to_user(&p->as, ulen, &n, 4) ? -E_FAULT : 0;
}

/* sockaddr out for a socket: AF_UNIX sockets report their path (or an unnamed address), the rest sockaddr_in */
static long put_name(struct proc *p, struct sock *sk, u64 ua, u64 ulen, u32 ip, u16 port, int peer)
{
    if (!sk || !sock_is_unix(sk)) return put_sockaddr(p, ua, ulen, ip, port);
    if (!ua || !ulen) return 0;
    u8 sa[112]; memset(sa, 0, sizeof sa); *(u16 *)sa = 1; u32 n = 2;
    if (!peer || port) for (int i = 0; i < UNIX_MAX; i++) if (unix_tab[i].port == port && port) {
        const char *s = unix_tab[i].path; u32 l = (u32)strlen(s);
        if (s[0] == '@') { memcpy(sa + 3, s + 1, l - 1); n = 2 + l; } else { memcpy(sa + 2, s, l + 1); n = 2 + l + 1; }
        break;
    }
    u32 have = 0; if (copy_from_user(&p->as, &have, ulen, 4)) return -E_FAULT;
    if (copy_to_user(&p->as, ua, sa, have < n ? have : n)) return -E_FAULT;
    return copy_to_user(&p->as, ulen, &n, 4) ? -E_FAULT : 0;
}

static long new_sock_fd(struct proc *p, struct sock *s, int flags)
{
    struct file *f = file_new(FT_SOCK, flags & SOCK_NONBLOCK_ ? O_NONBLOCK | 2 : 2);
    if (!f) { sock_free(s); return -E_NOMEM; }
    f->sock = s;
    long fd = fdt_install(p->fds, f, 0, (flags & SOCK_CLOEXEC_) != 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long sys_socket(struct proc *p, int domain, int type, int proto)
{
    (void)proto;
    if (domain != 2 && domain != 1) return -97;                         /* AF_INET and AF_UNIX (AF_INET6 / netlink / packet: EAFNOSUPPORT, callers fall back) */
    int t = type & 0xF;
    if (t == 5) t = 1;                                                  /* SOCK_SEQPACKET: carried as a stream */
    if (t != 1 && t != 2) return -94;                                   /* ESOCKTNOSUPPORT */
    struct sock *s = sock_new(t == 1 ? SOCK_STREAM_ : SOCK_DGRAM_);
    if (s && domain == 1) sock_set_unix(s, 1);
    return s ? new_sock_fd(p, s, type) : -E_NOMEM;
}

static long sys_sendto(struct proc *p, int fd, u64 ubuf, u64 len, int flags, u64 uaddr, u64 alen)
{
    struct file *sf; struct sock *s = fd_sock(p, fd, &sf); if (!s) return fdt_get(p->fds, fd) ? -88 : -E_BADF;
    int nb = (sf->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT_);
    u32 ip = 0; u16 port = 0; int hasd = 0;
    if (uaddr) { long r = get_sockaddr(p, uaddr, alen, &ip, &port); if (r) return r; hasd = 1; }
    size_t chunk = len < 32768 ? len : 32768; if (!chunk) return sock_send(s, "", 0, hasd, ip, port, nb);
    u8 *b = kmalloc(chunk); if (!b) return -E_NOMEM;
    long total = 0;
    while ((u64)total < len) {
        size_t c = len - (u64)total < chunk ? len - (u64)total : chunk;
        if (copy_from_user(&p->as, b, ubuf + (u64)total, c)) { kfree(b); return total ? total : -E_FAULT; }
        long r = sock_send(s, b, c, hasd, ip, port, nb);
        if (r < 0) { kfree(b); return total ? total : r; }
        total += r;
        if (!sock_is_stream(s) || (size_t)r < c) break;                  /* datagram, or a non-blocking stream that took only part */
    }
    kfree(b);
    return total;
}

static long sys_recvfrom(struct proc *p, int fd, u64 ubuf, u64 len, int flags, u64 uaddr, u64 ualen)
{
    struct file *f; struct sock *s = fd_sock(p, fd, &f); if (!s) return fdt_get(p->fds, fd) ? -88 : -E_BADF;
    size_t c = len < 32768 ? len : 32768; u8 *b = kmalloc(c ? c : 1); if (!b) return -E_NOMEM;
    u32 sip = 0; u16 sport = 0;
    long r = sock_recv(s, b, c, (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT_), &sip, &sport);
    if (r > 0 && copy_to_user(&p->as, ubuf, b, (size_t)r)) r = -E_FAULT;
    kfree(b);
    if (r >= 0 && uaddr) { long e = put_name(p, s, uaddr, ualen, sip, sport, 1); if (e) return e; }
    return r;
}

struct umsghdr { u64 name; u32 namelen, pad; u64 iov; u64 iovlen; u64 ctl; u64 ctllen; int flags; };
static long sys_sendmsg(struct proc *p, int fd, u64 umsg, int flags)
{
    struct umsghdr m; if (copy_from_user(&p->as, &m, umsg, sizeof m)) return -E_FAULT;
    if (m.iovlen > 16) return -E_INVAL;
    u8 *b = kmalloc(32768); if (!b) return -E_NOMEM;
    size_t tot = 0;
    for (u64 i = 0; i < m.iovlen; i++) {
        u64 iv[2]; if (copy_from_user(&p->as, iv, m.iov + i * 16, 16)) { kfree(b); return -E_FAULT; }
        if (tot + iv[1] > 32768) iv[1] = 32768 - tot;
        if (iv[1] && copy_from_user(&p->as, b + tot, iv[0], iv[1])) { kfree(b); return -E_FAULT; }
        tot += iv[1];
    }
    u32 ip = 0; u16 port = 0; int hasd = 0;
    struct sock *s = fd_sock(p, fd, NULL);
    long r;
    if (!s) r = fdt_get(p->fds, fd) ? -88 : -E_BADF;
    else if (m.name && m.namelen && (r = get_sockaddr(p, m.name, m.namelen, &ip, &port)) != 0) { }
    else { hasd = m.name != 0; r = sock_send(s, b, tot, hasd, ip, port, 0); }
    (void)flags; kfree(b);
    return r;
}
static long sys_recvmsg(struct proc *p, int fd, u64 umsg, int flags)
{
    struct umsghdr m; if (copy_from_user(&p->as, &m, umsg, sizeof m)) return -E_FAULT;
    if (m.iovlen > 16) return -E_INVAL;
    struct file *f; struct sock *s = fd_sock(p, fd, &f); if (!s) return fdt_get(p->fds, fd) ? -88 : -E_BADF;
    u8 *b = kmalloc(32768); if (!b) return -E_NOMEM;
    u32 sip = 0; u16 sport = 0;
    long r = sock_recv(s, b, 32768, (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT_), &sip, &sport);
    if (r > 0) {
        size_t off = 0;
        for (u64 i = 0; i < m.iovlen && off < (size_t)r; i++) {
            u64 iv[2]; if (copy_from_user(&p->as, iv, m.iov + i * 16, 16)) { r = -E_FAULT; break; }
            size_t c = (size_t)r - off < iv[1] ? (size_t)r - off : iv[1];
            if (c && copy_to_user(&p->as, iv[0], b + off, c)) { r = -E_FAULT; break; }
            off += c;
        }
        if (m.name && m.namelen >= 16) { u8 sa[16]; memset(sa, 0, 16); *(u16 *)sa = 2; sa[2] = (u8)(sport >> 8); sa[3] = (u8)sport; sa[4] = (u8)(sip >> 24); sa[5] = (u8)(sip >> 16); sa[6] = (u8)(sip >> 8); sa[7] = (u8)sip; copy_to_user(&p->as, m.name, sa, 16); }
        u32 nl = 16, zero = 0; copy_to_user(&p->as, umsg + 8, &nl, 4); copy_to_user(&p->as, umsg + 40, &zero, 8); copy_to_user(&p->as, umsg + 48, &zero, 4);
    }
    kfree(b);
    return r;
}

static long sys_sockopt(struct proc *p, int fd, int level, int opt, u64 uval, u64 ulen, int get)
{
    struct sock *s = fd_sock(p, fd, NULL); if (!s) return fdt_get(p->fds, fd) ? -88 : -E_BADF;
    if (!get) return 0;                                                 /* options are accepted and ignored */
    int v = 0;
    if (level == 1 && opt == 3) v = sock_is_stream(s) ? 1 : 2;          /* SO_TYPE */
    else if (level == 1 && (opt == 7 || opt == 8)) v = 65536;           /* SO_SNDBUF / SO_RCVBUF */
    u32 n = 4; if (!uval || !ulen) return -E_FAULT;
    if (copy_to_user(&p->as, uval, &v, 4) || copy_to_user(&p->as, ulen, &n, 4)) return -E_FAULT;
    return 0;
}

static long sys_epoll_wait(struct proc *p, int epfd, u64 uev, int max, long timeout_ms)
{
    struct file *ep = fdt_get(p->fds, epfd);
    if (!ep || ep->type != FT_EPOLL) return -E_BADF;
    if (max <= 0) return -E_INVAL;
    if (max > 64) max = 64;
    struct epoll_ev ev[64];
    u64 deadline = timeout_ms < 0 ? ~0ull : ticks + ms_to_ticks((u64)timeout_ms);
    for (;;) {
        int n = epoll_collect(ep, p->fds, ev, max);
        if (n) return copy_to_user(&p->as, uev, ev, (size_t)n * sizeof(struct epoll_ev)) ? -E_FAULT : n;
        if (timeout_ms == 0 || (deadline != ~0ull && ticks >= deadline)) return 0;
        if (sig_interrupted(p)) return -E_INTR;
        extern int tty_pump(void);
        tty_pump();
        thread_sleep_ms(1);
    }
}

/* ------------------------------------------------------------ the dispatcher */
i64 neos_tool_call(struct proc *p, u64 cmd, u64 out, u64 outlen);
long neos_tool_call_ext(struct proc *p, u64 cmd, u64 out, u64 outlen);
#define RET(v) do { f->rax = (u64)(long)(v); return f; } while (0)

static struct frame *linux_syscall_impl(struct frame *f);
static struct tunable *t_strace;

struct frame *linux_syscall(struct frame *f)
{
    if (!t_strace->val) return linux_syscall_impl(f);
    struct proc *p = proc_current();
    u64 nr = f->rax, a0 = f->rdi, a1 = f->rsi, a2 = f->rdx;
    kprintf("\x1b[90m[strace %d] %lu(%lx, %lx, %lx)\x1b[0m", p ? p->pid : 0, nr, a0, a1, a2);
    struct frame *r = linux_syscall_impl(f);
    if (r == f) kprintf("\x1b[90m = %ld\x1b[0m\n", (long)f->rax); else kprintf("\n");
    return r;
}

static struct frame *linux_syscall_impl(struct frame *f)
{
    struct proc *p = proc_current();
    if (!p) panic("syscall without a process");
    u64 nr = f->rax, a0 = f->rdi, a1 = f->rsi, a2 = f->rdx, a3 = f->r10, a4 = f->r8, a5 = f->r9;
    char abs[256], abs2[256];
    long r;
    p->syscalls++;
    uctx_current()->last_sys = (int)nr;

    switch (nr) {
    case 0:  { struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF); RET(rw_user(p, x, a1, a2, 0)); }
    case 1:  { struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF); RET(rw_user(p, x, a1, a2, 1)); }
    case 2:  if ((r = get_path(p, a0, abs, sizeof abs))) RET(r); RET(do_open(p, abs, (int)a1, (u32)a2));
    case 257: if ((r = path_at(p, (long)a0, a1, abs, sizeof abs))) RET(r); RET(do_open(p, abs, (int)a2, (u32)a3));
    case 3:  RET(fdt_close(p->fds, (int)a0));
    case 4: case 6: { if ((r = get_path(p, a0, abs, sizeof abs))) RET(r); struct inode *i = vfs_lookup(abs); if (nr == 4) i = follow(i, 0); if (!i) RET(-E_NOENT); RET(stat_inode(p, i, a1)); }
    case 5:  { struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF); RET(stat_file(p, x, a1)); }
    case 262: {                                                           /* newfstatat(dirfd, path, st, flags) */
        char raw[8]; int l = strncpy_from_user(&p->as, raw, a1, sizeof raw);
        if (l == 0 && (a3 & 0x1000)) { struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF); RET(stat_file(p, x, a2)); }
        if ((r = path_at(p, (long)a0, a1, abs, sizeof abs))) RET(r);
        struct inode *i = vfs_lookup(abs); if (!(a3 & 0x100)) i = follow(i, 0);
        if (!i) RET(-E_NOENT);
        RET(stat_inode(p, i, a2));
    }
    case 8:  {
        struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF);
        if (x->type != FT_INODE && x->type != FT_DIR) RET(-E_SPIPE);
        i64 base = a2 == 0 ? 0 : a2 == 1 ? x->off : (i64)x->ino->size;
        i64 np = base + (i64)a1;
        if (np < 0) RET(-E_INVAL);
        x->off = np; RET(np);
    }
    case 17: case 18: {                                                   /* pread64 / pwrite64 */
        struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF);
        if (x->type != FT_INODE) RET(-E_SPIPE);
        i64 save = x->off; x->off = (i64)a3;
        r = rw_user(p, x, a1, a2, nr == 18); x->off = save; RET(r);
    }
    case 19: RET(sys_readv(p, (int)a0, a1, (long)a2, 0));
    case 20: RET(sys_readv(p, (int)a0, a1, (long)a2, 1));
    case 7:  RET(sys_poll(p, a0, a1, (int)a2));
    case 23: RET(sys_select(p, (int)a0, a1, a2, a3, a4, 0));
    case 270: RET(sys_select(p, (int)a0, a1, a2, a3, a4, 1));
    case 436: {                                                           /* close_range(first, last, flags): subprocess/spawn use it to close everything above stderr */
        u64 lo = a0, hi = a1 < MAX_FDS ? a1 : MAX_FDS - 1;
        for (u64 fd = lo; fd <= hi; fd++) {
            if (!fdt_get(p->fds, (int)fd)) continue;
            if (a2 & 4) p->fds->cloexec[fd] = 1; else fdt_close(p->fds, (int)fd);
        }
        RET(0);
    }
    case 40: {                                                            /* sendfile(out, in, *offset, count) */
        struct file *o = fdt_get(p->fds, (int)a0), *in = fdt_get(p->fds, (int)a1);
        if (!o || !in) RET(-E_BADF);
        i64 save = in->off; int useoff = a2 != 0;
        if (useoff) { u64 off; if (copy_from_user(&p->as, &off, a2, 8)) RET(-E_FAULT); in->off = (i64)off; }
        u8 *kb = kmalloc(32768); if (!kb) RET(-E_NOMEM);
        long total = 0;
        while ((u64)total < a3) {
            size_t c = a3 - (u64)total < 32768 ? (size_t)(a3 - (u64)total) : 32768;
            long rd = file_read(in, kb, c); if (rd <= 0) { if (!total && rd < 0) total = rd; break; }
            long wr = file_write(o, kb, (size_t)rd); if (wr < 0) { if (!total) total = wr; break; }
            total += wr; if (wr < rd) break;
        }
        kfree(kb);
        if (useoff && total > 0) { u64 no = (u64)in->off; copy_to_user(&p->as, a2, &no, 8); in->off = save; }
        RET(total);
    }
    case 41: RET(sys_socket(p, (int)a0, (int)a1, (int)a2));
    case 42: { struct sock *sk = fd_sock(p, (int)a0, NULL); if (!sk) RET(fdt_get(p->fds, (int)a0) ? -88 : -E_BADF); u32 ip; u16 pt; if ((r = get_sockaddr(p, a1, a2, &ip, &pt))) RET(r); RET(sock_connect(sk, ip, pt)); }
    case 49: { struct sock *sk = fd_sock(p, (int)a0, NULL); if (!sk) RET(fdt_get(p->fds, (int)a0) ? -88 : -E_BADF);
        { u16 fam = 0; if (a2 >= 2 && !copy_from_user(&p->as, &fam, a1, 2) && fam == 1) RET(unix_bind(p, sk, a1, a2)); } u32 ip; u16 pt; if ((r = get_sockaddr(p, a1, a2, &ip, &pt))) RET(r); RET(sock_bind(sk, ip, pt)); }
    case 50: { struct sock *sk = fd_sock(p, (int)a0, NULL); if (!sk) RET(fdt_get(p->fds, (int)a0) ? -88 : -E_BADF); RET(sock_listen(sk, (int)a1)); }
    case 43: case 288: {
        struct file *lf; struct sock *sk = fd_sock(p, (int)a0, &lf); if (!sk) RET(fdt_get(p->fds, (int)a0) ? -88 : -E_BADF);
        struct sock *ns; r = sock_accept(sk, (lf->flags & O_NONBLOCK) != 0, &ns); if (r) RET(r);
        u32 lip, rip; u16 lp, rp; sock_names(ns, &lip, &lp, &rip, &rp);
        if (sock_is_unix(sk)) sock_set_unix(ns, 1);
        if (a1 && (r = put_name(p, ns, a1, a2, rip, rp, 1))) { sock_free(ns); RET(r); }
        RET(new_sock_fd(p, ns, nr == 288 ? (int)a3 : 0));
    }
    case 44: RET(sys_sendto(p, (int)a0, a1, a2, (int)a3, a4, a5));
    case 45: RET(sys_recvfrom(p, (int)a0, a1, a2, (int)a3, a4, a5));
    case 46: RET(sys_sendmsg(p, (int)a0, a1, (int)a2));
    case 47: RET(sys_recvmsg(p, (int)a0, a1, (int)a2));
    case 307: { long done = 0; for (u64 i = 0; i < a2; i++) { long q = sys_sendmsg(p, (int)a0, a1 + i * 64, (int)a3); if (q < 0) { if (!done) RET(q); break; } copy_to_user(&p->as, a1 + i * 64 + 56, &q, 4); done++; } RET(done); }
    case 299: { long q = sys_recvmsg(p, (int)a0, a1, (int)a3 | MSG_DONTWAIT_ * 0); if (q < 0) RET(q); copy_to_user(&p->as, a1 + 56, &q, 4); RET(1); }
    case 48: { struct sock *sk = fd_sock(p, (int)a0, NULL); if (!sk) RET(fdt_get(p->fds, (int)a0) ? -88 : -E_BADF); RET(sock_shutdown(sk, (int)a1)); }
    case 51: case 52: { struct sock *sk = fd_sock(p, (int)a0, NULL); if (!sk) RET(fdt_get(p->fds, (int)a0) ? -88 : -E_BADF); u32 lip, rip; u16 lp, rp; sock_names(sk, &lip, &lp, &rip, &rp); if (nr == 52 && !rp) RET(-107); RET(nr == 51 ? put_name(p, sk, a1, a2, lip, lp, 0) : put_name(p, sk, a1, a2, rip, rp, 1)); }
    case 54: RET(sys_sockopt(p, (int)a0, (int)a1, (int)a2, a3, a4, 0));
    case 55: RET(sys_sockopt(p, (int)a0, (int)a1, (int)a2, a3, a4, 1));
    case 53: {                                                            /* socketpair(AF_UNIX): a connected loopback pair */
        if (a0 != 1) RET(-97);
        struct sock *ls = sock_new(SOCK_STREAM_), *cs = sock_new(SOCK_STREAM_), *as = NULL;
        if (!ls || !cs) { if (ls) sock_free(ls); if (cs) sock_free(cs); RET(-E_NOMEM); }
        u16 port = unix_next++; if (unix_next >= 30000) unix_next = 20000;
        if ((r = sock_bind(ls, 0x7F000001, port)) || (r = sock_listen(ls, 1)) || (r = sock_connect(cs, 0x7F000001, port)) || (r = sock_accept(ls, 0, &as))) {
            sock_free(ls); sock_free(cs); RET(r);
        }
        sock_free(ls); sock_set_unix(cs, 1); sock_set_unix(as, 1);
        long fa = new_sock_fd(p, cs, (int)a1); if (fa < 0) { sock_free(as); RET(fa); }
        long fb = new_sock_fd(p, as, (int)a1); if (fb < 0) { fdt_close(p->fds, (int)fa); RET(fb); }
        int sv[2] = { (int)fa, (int)fb };
        if (copy_to_user(&p->as, a3, sv, sizeof sv)) RET(-E_FAULT);
        RET(0);
    }
    case 213: case 291: { struct file *ef = file_new(FT_EPOLL, 2); if (!ef) RET(-E_NOMEM); ef->ep = kmalloc(2048); if (!ef->ep) { kfree(ef); RET(-E_NOMEM); } memset(ef->ep, 0, 2048); r = fdt_install(p->fds, ef, 0, nr == 291 && (a0 & SOCK_CLOEXEC_)); if (r < 0) file_put(ef); RET(r); }
    case 233: { struct file *ef = fdt_get(p->fds, (int)a0); if (!ef || ef->type != FT_EPOLL) RET(-E_BADF); struct epoll_ev ev = { 0, 0 }; if (a1 != 2 && copy_from_user(&p->as, &ev, a3, sizeof ev)) RET(-E_FAULT); RET(epoll_ctl_op(ef, p->fds, (int)a1, (int)a2, ev.events, ev.data)); }
    case 232: case 281: RET(sys_epoll_wait(p, (int)a0, a1, (int)a2, (int)a3));
    case 319: {                                                           /* memfd_create: an unlinked file that lives while it is open or mapped */
        static u32 memfd_n; char path[48]; ksnprintf(path, sizeof path, "/dev/shm/.memfd-%u", ++memfd_n);
        long fd = do_open(p, path, 0102 | 0200 | 2, 0600);                /* O_CREAT | O_EXCL | O_RDWR */
        if (fd >= 0) { vfs_unlink(path); if (a1 & 1) p->fds->cloexec[fd] = 1; }
        RET(fd);
    }
    case 285: { struct file *x = fdt_get(p->fds, (int)a0); if (!x || x->type != FT_INODE) RET(-E_BADF); if (!(a1 & 1) && a2 + a3 > x->ino->size) inode_truncate(x->ino, a2 + a3); RET(0); }   /* fallocate */
    case 118: case 120: { u32 z = 0; if ((a0 && copy_to_user(&p->as, a0, &z, 4)) || (a1 && copy_to_user(&p->as, a1, &z, 4)) || (a2 && copy_to_user(&p->as, a2, &z, 4))) RET(-E_FAULT); RET(0); }   /* getresuid / getresgid */
    case 141: RET(0);                                                     /* setpriority */
    case 253: case 294: { struct file *ef = file_new(FT_EVENTFD, 2); if (!ef) RET(-E_NOMEM); r = fdt_install(p->fds, ef, 0, nr == 294 && (a0 & SOCK_CLOEXEC_)); if (r < 0) file_put(ef); RET(r); }   /* inotify_init(1): a descriptor that never reports a change */
    case 254: RET(1);                                                     /* inotify_add_watch: accepted, nothing is ever reported */
    case 255: RET(0);
    case 222: case 223: case 224: case 225: case 226: RET(-E_NOSYS);      /* POSIX timers: callers fall back to timerfd / sleeping */
    case 444: case 330: case 331: RET(-E_NOSYS);                          /* landlock, pkeys */
    case 284: case 290: { struct file *ef = file_new(FT_EVENTFD, (nr == 290 && (a1 & SOCK_NONBLOCK_)) ? O_NONBLOCK | 2 : 2); if (!ef) RET(-E_NOMEM); ef->evcount = a0; r = fdt_install(p->fds, ef, 0, nr == 290 && (a1 & SOCK_CLOEXEC_)); if (r < 0) file_put(ef); RET(r); }

    case 271: { long to = -1; if (a2) { u64 ts[2]; if (copy_from_user(&p->as, ts, a2, 16)) RET(-E_FAULT); to = (long)(ts[0] * 1000 + ts[1] / 1000000); } RET(sys_poll(p, a0, a1, to)); }
    case 9:  RET(sys_mmap(p, a0, a1, (int)a2, (int)a3, (long)(int)a4, a5));
    case 10: RET(vm_mprotect(&p->as, a0, a1, (int)a2) ? -E_NOMEM : 0);
    case 11: RET(vm_munmap(&p->as, a0, a1) ? -E_INVAL : 0);
    case 12: RET(sys_brk(p, a0));
    case 25: RET(-E_NOMEM);                                               /* mremap: callers fall back to mmap+copy */
    case 137: case 138: {                                                 /* statfs / fstatfs: a memory-backed filesystem */
        if (nr == 137) { if ((r = get_path(p, a0, abs, sizeof abs))) RET(r); if (!vfs_lookup(abs)) RET(-E_NOENT); }
        else if (!fdt_get(p->fds, (int)a0)) RET(-E_BADF);
        u64 sf[15]; memset(sf, 0, sizeof sf);
        sf[0] = 0x01021994; sf[1] = 4096; sf[2] = pmm_total_pages(); sf[3] = pmm_free_pages(); sf[4] = pmm_free_pages();
        sf[5] = 8192; sf[6] = 4096; sf[8] = 255; sf[9] = 4096;
        RET(copy_to_user(&p->as, nr == 137 ? a1 : a1, sf, sizeof sf) ? -E_FAULT : 0);
    }
    case 26: { struct vma *v = vma_find(&p->as, a0); if (v && v->shm) shm_sync(v->shm); RET(0); }   /* msync: write a shared file mapping back */
    case 28: case 149: case 150: case 27: RET(0);                         /* madvise, mlock, munlock, mincore */
    case 13: {                                                            /* rt_sigaction */
        if (a0 < 1 || a0 > 64 || a0 == 9 || a0 == 19) RET(-E_INVAL);
        if (a2) { u64 o[4] = { p->sig[a0].handler, p->sig[a0].flags, p->sig[a0].restorer, p->sig[a0].mask }; if (copy_to_user(&p->as, a2, o, 32)) RET(-E_FAULT); }
        if (a1) { u64 nw[4]; if (copy_from_user(&p->as, nw, a1, 32)) RET(-E_FAULT); p->sig[a0] = (struct sigact){ nw[0], nw[1], nw[3], nw[2] }; }
        RET(0);
    }
    case 14: {                                                            /* rt_sigprocmask(how, set, oldset, sigsetsize) */
        u64 old = p->sigmask;
        if (a2 && copy_to_user(&p->as, a2, &old, 8)) RET(-E_FAULT);
        if (a1) {
            u64 set; if (copy_from_user(&p->as, &set, a1, 8)) RET(-E_FAULT);
            set &= ~((1ull << 8) | (1ull << 18));                         /* SIGKILL, SIGSTOP can never be blocked */
            if (a0 == 0) p->sigmask |= set; else if (a0 == 1) p->sigmask &= ~set; else if (a0 == 2) p->sigmask = set; else RET(-E_INVAL);
            if (p->sigpending & ~p->sigmask) sig_any = 1;
        }
        RET(0);
    }
    case 131: RET(0);                                                     /* sigaltstack: handlers run on the thread's own stack */
    case 127: { u64 pend = p->sigpending & p->sigmask; if (a0 && copy_to_user(&p->as, a0, &pend, 8)) RET(-E_FAULT); RET(0); }
    case 130: {                                                           /* rt_sigsuspend: swap the mask, wait for a signal, EINTR */
        u64 set; if (copy_from_user(&p->as, &set, a0, 8)) RET(-E_FAULT);
        p->susp_mask = p->sigmask; p->susp = 1;
        p->sigmask = set & ~((1ull << 8) | (1ull << 18));
        if (p->sigpending & ~p->sigmask) sig_any = 1;
        while (!sig_interrupted(p)) thread_sleep_ms(2);
        RET(-E_INTR);
    }
    case 15: { long sr = sig_return(p, f); if (sr) RET(sr); return f; }   /* rt_sigreturn: the frame was restored from the user's ucontext */
    case 37: { u64 rem = 0; if (p->alarm_at > ticks) rem = (p->alarm_at - ticks) * 1000 / timer_hz() / 1000 + 1; alarm_set(p, a0 ? ticks + a0 * timer_hz() : 0, 0); RET(rem); }
    case 38: {                                                            /* setitimer(which, new, old): ITIMER_REAL only */
        if (a0 != 0) RET(0);
        if (a2) { u64 o[4] = { 0, 0, 0, 0 }; if (p->alarm_at > ticks) { u64 us = (p->alarm_at - ticks) * 1000000ull / timer_hz(); o[2] = us / 1000000; o[3] = us % 1000000; } if (p->alarm_int) { u64 us = p->alarm_int * 1000000ull / timer_hz(); o[0] = us / 1000000; o[1] = us % 1000000; } copy_to_user(&p->as, a2, o, 32); }
        if (a1) { u64 n[4]; if (copy_from_user(&p->as, n, a1, 32)) RET(-E_FAULT);
            u64 val = n[2] * timer_hz() + n[3] * timer_hz() / 1000000, per = n[0] * timer_hz() + n[1] * timer_hz() / 1000000;
            if ((n[2] || n[3]) && !val) val = 1; if ((n[0] || n[1]) && !per) per = 1;
            alarm_set(p, val ? ticks + val : 0, per); }
        RET(0);
    }
    case 36: { if (a0 == 0 && a1) { u64 o[4] = { 0, 0, 0, 0 }; if (p->alarm_at > ticks) { u64 us = (p->alarm_at - ticks) * 1000000ull / timer_hz(); o[2] = us / 1000000; o[3] = us % 1000000; } copy_to_user(&p->as, a1, o, 32); } RET(0); }
    case 16: RET(sys_ioctl(p, (int)a0, a1, a2));
    case 21: case 269: case 439: { if ((r = nr == 21 ? get_path(p, a0, abs, sizeof abs) : path_at(p, (long)a0, a1, abs, sizeof abs))) RET(r); RET(vfs_lookup(abs) ? 0 : -E_NOENT); }
    case 22: case 293: {
        struct file *rd, *wr;
        if ((r = pipe_create(&rd, &wr))) RET(r);
        if (nr == 293 && (a1 & O_NONBLOCK)) { rd->flags |= O_NONBLOCK; wr->flags |= O_NONBLOCK; }
        int cx = nr == 293 && (a1 & O_CLOEXEC);
        int fds[2] = { fdt_install(p->fds, rd, 0, cx), -1 };
        if (fds[0] >= 0) fds[1] = fdt_install(p->fds, wr, 0, cx);
        if (fds[0] < 0 || fds[1] < 0) { if (fds[0] >= 0) fdt_close(p->fds, fds[0]); else file_put(rd); file_put(wr); RET(-E_MFILE); }
        if (copy_to_user(&p->as, a0, fds, 8)) RET(-E_FAULT);
        RET(0);
    }
    case 24: thread_yield(); RET(0);
    case 32: { struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF); file_get(x); r = fdt_install(p->fds, x, 0, 0); if (r < 0) file_put(x); RET(r); }
    case 33: case 292: {
        struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF);
        if (a1 >= MAX_FDS) RET(-E_BADF);
        if ((int)a0 == (int)a1) RET(nr == 33 ? (long)a1 : -E_INVAL);
        file_get(x);
        if (p->fds->f[a1]) fdt_close(p->fds, (int)a1);
        p->fds->f[a1] = x; p->fds->cloexec[a1] = nr == 292 && (a2 & O_CLOEXEC);
        RET((long)a1);
    }
    case 72: {                                                            /* fcntl */
        struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF);
        switch (a1) {
        case 0: case 1030: file_get(x); r = fdt_install(p->fds, x, (int)a2, a1 == 1030); if (r < 0) file_put(x); RET(r);
        case 1: RET(p->fds->cloexec[a0] ? 1 : 0);
        case 2: p->fds->cloexec[a0] = (u8)(a2 & 1); RET(0);
        case 3: RET(x->flags);
        case 4: x->flags = (x->flags & ~(O_APPEND | O_NONBLOCK)) | (int)(a2 & (O_APPEND | O_NONBLOCK)); RET(0);
        case 5: case 6: case 7: RET(0);                                   /* advisory locks: single user */
        }
        RET(-E_INVAL);
    }
    case 73: case 74: case 75: RET(0);                       /* flock, fsync, fdatasync */
    case 77: { struct file *x = fdt_get(p->fds, (int)a0); if (!x || x->type != FT_INODE) RET(-E_BADF); RET(inode_truncate(x->ino, a1)); }
    case 76: if ((r = get_path(p, a0, abs, sizeof abs))) RET(r); { struct inode *i = vfs_lookup(abs); if (!i) RET(-E_NOENT); RET(inode_truncate(i, a1)); }
    case 78: case 217: RET(sys_getdents(p, (int)a0, a1, a2));
    case 79: { size_t l = strlen(p->cwd) + 1; if (a1 < l) RET(-E_RANGE); if (copy_to_user(&p->as, a0, p->cwd, l)) RET(-E_FAULT); RET((long)l); }
    case 80: { if ((r = get_path(p, a0, abs, sizeof abs))) RET(r); struct inode *i = follow(vfs_lookup(abs), 0); if (!i) RET(-E_NOENT); if ((i->mode & S_IFMT) != S_IFDIR) RET(-E_NOTDIR); strlcpy(p->cwd, i->path, sizeof p->cwd); RET(0); }
    case 81: { struct file *x = fdt_get(p->fds, (int)a0); if (!x) RET(-E_BADF); if (x->type != FT_DIR) RET(-E_NOTDIR); strlcpy(p->cwd, x->ino->path, sizeof p->cwd); RET(0); }
    case 82: case 264: case 316: {
        long dfa = nr == 82 ? AT_FDCWD : (long)a0; u64 op = nr == 82 ? a0 : a1, np = nr == 82 ? a1 : a3; long dfb = nr == 82 ? AT_FDCWD : (long)a2;
        if ((r = path_at(p, dfa, op, abs, sizeof abs)) || (r = path_at(p, dfb, np, abs2, sizeof abs2))) RET(r);
        RET(vfs_rename(abs, abs2));
    }
    case 83: case 258: if ((r = nr == 83 ? get_path(p, a0, abs, sizeof abs) : path_at(p, (long)a0, a1, abs, sizeof abs))) RET(r); RET(vfs_mkdir(abs, (u32)(nr == 83 ? a1 : a2)));
    case 84: if ((r = get_path(p, a0, abs, sizeof abs))) RET(r); RET(vfs_rmdir(abs));
    case 87: if ((r = get_path(p, a0, abs, sizeof abs))) RET(r); RET(vfs_unlink(abs));
    case 263: if ((r = path_at(p, (long)a0, a1, abs, sizeof abs))) RET(r); RET((a2 & 0x200) ? vfs_rmdir(abs) : vfs_unlink(abs));
    case 88: case 266: {
        char tgt[100]; if (strncpy_from_user(&p->as, tgt, a0, sizeof tgt) < 0) RET(-E_FAULT);
        if ((r = nr == 88 ? get_path(p, a1, abs, sizeof abs) : path_at(p, (long)a1, a2, abs, sizeof abs))) RET(r);
        RET(vfs_symlink(tgt, abs));
    }
    case 89: case 267: {
        if ((r = nr == 89 ? get_path(p, a0, abs, sizeof abs) : path_at(p, (long)a0, a1, abs, sizeof abs))) RET(r);
        u64 ubuf = nr == 89 ? a1 : a2, bsz = nr == 89 ? a2 : a3;
        const char *t;
        if (!strcmp(abs, "/proc/self/exe")) t = p->exe;
        else if (!strcmp(abs, "/proc/self/cwd")) t = p->cwd;
        else { struct inode *i = vfs_lookup(abs); if (!i) RET(-E_NOENT); if ((i->mode & S_IFMT) != S_IFLNK) RET(-E_INVAL); t = i->link; }
        size_t l = strlen(t); if (l > bsz) l = bsz;
        RET(copy_to_user(&p->as, ubuf, t, l) ? -E_FAULT : (long)l);
    }
    case 90: case 91: case 268: {
        struct inode *i;
        if (nr == 91) { struct file *x = fdt_get(p->fds, (int)a0); if (!x || !x->ino) RET(-E_BADF); i = x->ino; }
        else { if ((r = nr == 90 ? get_path(p, a0, abs, sizeof abs) : path_at(p, (long)a0, a1, abs, sizeof abs))) RET(r); i = vfs_lookup(abs); if (!i) RET(-E_NOENT); }
        i->mode = (i->mode & S_IFMT) | ((u32)(nr == 268 ? a2 : nr == 90 ? a1 : a1) & 07777);
        RET(0);
    }
    case 92: case 93: case 94: case 260: case 280: case 132: case 235: RET(0);     /* chown*, utime*: no ownership model */
    case 95: RET(022);
    case 96: { u64 ns = wall_ns(); u64 tv[2] = { ns / 1000000000ull, ns / 1000 % 1000000 }; if (a0 && copy_to_user(&p->as, a0, tv, 16)) RET(-E_FAULT); RET(0); }
    case 201: { u64 s = wall_ns() / 1000000000ull; if (a0 && copy_to_user(&p->as, a0, &s, 8)) RET(-E_FAULT); RET((long)s); }
    case 228: RET(sys_clock_gettime(p, (int)a0, a1));
    case 229: { u64 ts[2] = { 0, 1 }; RET(a1 && copy_to_user(&p->as, a1, ts, 16) ? -E_FAULT : 0); }
    case 35: RET(sys_nanosleep(p, a0, a1));
    case 230: { if (a1 & 1) { u64 d = ts_to_deadline(p, a2, 1); if (d) thread_sleep_until(d); RET(0); } RET(sys_nanosleep(p, a2, a3)); }
    case 39: RET(p->pid);
    case 186: RET(uctx_current()->utid);
    case 110: RET(p->ppid);
    case 102: case 104: case 107: case 108: case 105: case 106: case 113: case 114: case 117: case 119: case 122: case 123: RET(0);   /* uid/gid: everything is root */
    case 115: RET(0);
    case 109: case 121: case 111: case 112: case 124: RET(p->pid);          /* setpgid, getpgid, getpgrp, setsid, getsid */
    case 63: RET(sys_uname(p, a0));
    case 97: case 98: { u64 rl[2] = { ~0ull, ~0ull }; if (a0 == 3) rl[0] = 8ull << 20; if (a0 == 7) rl[0] = MAX_FDS; if (a1 && copy_to_user(&p->as, nr == 97 ? a1 : a1, rl, 16)) RET(-E_FAULT); RET(0); }
    case 302: { u64 rl[2] = { ~0ull, ~0ull }; if (a1 == 3) rl[0] = 8ull << 20; if (a1 == 7) rl[0] = MAX_FDS; if (a3 && copy_to_user(&p->as, a3, rl, 16)) RET(-E_FAULT); RET(0); }
    case 99: {
        u64 si[14] = { time_ns() / 1000000000ull, 0, 0, 0, (u64)pmm_total_pages() * 4096, (u64)pmm_free_pages() * 4096, 0, 0, 0, 0, 0, 0, 0, 1 };
        u8 buf[112]; memset(buf, 0, sizeof buf); memcpy(buf, si, 14 * 8 > 112 ? 112 : 14 * 8);
        *(u16 *)(buf + 80) = (u16)MAX_PROCS; *(u32 *)(buf + 104) = 1;
        RET(copy_to_user(&p->as, a0, buf, 112) ? -E_FAULT : 0);
    }
    case 100: RET((long)ticks);
    case 157: RET(a0 == 15 || a0 == 16 || a0 == 38 || a0 == 4 || a0 == 3 ? 0 : -E_INVAL);                      /* prctl */
    case 158: {                                                           /* arch_prctl */
        if (a0 == 0x1002) { sched_set_fsbase(a1); RET(0); }
        if (a0 == 0x1001) { sched_set_gsbase(a1); RET(0); }
        if (a0 == 0x1004) { u64 v = uctx_current()->gsbase; RET(copy_to_user(&p->as, a1, &v, 8) ? -E_FAULT : 0); }
        if (a0 == 0x1003) { u64 v = uctx_current()->fsbase; RET(copy_to_user(&p->as, a1, &v, 8) ? -E_FAULT : 0); }
        RET(-E_INVAL);
    }
    case 218: uctx_current()->clear_tid = a0; RET(uctx_current()->utid);
    case 273: RET(0);                                                     /* set_robust_list */
    case 334: RET(-E_NOSYS);                                              /* rseq */
    case 318: { if (!a1) RET(0); size_t n = a1 < 256 ? a1 : 256; u8 b[256]; fill_random(b, n); RET(copy_to_user(&p->as, a0, b, n) ? -E_FAULT : (long)n); }
    case 202: RET(sys_futex(p, a0, (int)a1, (u32)a2, a3, a4, (u32)a5));
    case 204: { u64 m = 1; if (a2 && a1 >= 8 && copy_to_user(&p->as, a2, &m, 8)) RET(-E_FAULT); RET(8); }
    case 203: RET(0);
    case 142: case 144: {                                                 /* sched_setparam / sched_setscheduler: SCHED_FIFO / SCHED_RR priorities 1..99 map onto kernel priorities 1..3, above ordinary processes (4) */
        if (a0 && (int)a0 != p->pid) RET(-3);
        int pol = nr == 144 ? (int)a1 : 1; u64 pa = nr == 144 ? a2 : a1; int lp = 0;
        if (copy_from_user(&p->as, &lp, pa, 4)) RET(-E_FAULT);
        int kp;
        if (pol == 1 || pol == 2) { if (lp < 1 || lp > 99) RET(-E_INVAL); kp = lp >= 67 ? 1 : lp >= 34 ? 2 : 3; }
        else if (pol == 0 || pol == 3) { if (lp) RET(-E_INVAL); kp = 4; }
        else if (pol == 5) kp = 6;
        else RET(-E_INVAL);
        RET(thread_set_prio(thread_current_id(), kp) ? -E_INVAL : 0);
    }
    case 143: case 145: {
        if (a0 && (int)a0 != p->pid) RET(-3);
        int kp = thread_prio_of(thread_current_id());
        if (nr == 145) RET(kp >= 1 && kp <= 3 ? 1 : kp > 4 ? 5 : 0);
        int lp = kp == 1 ? 99 : kp == 2 ? 66 : kp == 3 ? 33 : 0;
        RET(copy_to_user(&p->as, a1, &lp, 4) ? -E_FAULT : 0);
    }
    case 146: RET(a0 == 1 || a0 == 2 ? 99 : 0);
    case 147: RET(a0 == 1 || a0 == 2 ? 1 : 0);
    case 148: { u64 ts[2] = { 0, 10000000 }; RET(a1 && copy_to_user(&p->as, a1, ts, 16) ? -E_FAULT : 0); }
    case 151: case 152: case 325: RET(0);                                 /* mlockall / munlockall / mlock2: memory is never paged out, so it is always locked */
    case 86: case 265: {                                                  /* link / linkat: a copy (NeoFS has no hard links) */
        if (nr == 86) { if ((r = get_path(p, a0, abs, sizeof abs)) || (r = get_path(p, a1, abs2, sizeof abs2))) RET(r); }
        else if ((r = path_at(p, (long)a0, a1, abs, sizeof abs)) || (r = path_at(p, (long)a2, a3, abs2, sizeof abs2))) RET(r);
        const u8 *ld; size_t ln;
        if (vfs_lookup(abs2)) RET(-17);
        if (fs_read(abs, &ld, &ln)) RET(-E_NOENT);
        RET(fs_write(abs2, ld, ln) ? -E_NOMEM : 0);
    }
    case 200: case 234: { int sg = (int)(nr == 200 ? a1 : a2); if (sg == 23 || sg == 0) RET(0); RET(proc_signal(p->pid, sg)); }   /* tkill/tgkill: delivered to the process (SIGURG, Go's preemption nudge, is dropped) */
    case 62: {
        long pid = (long)a0;
        if (pid > 0) {
            r = proc_signal((int)pid, (int)a1);
            if (p->state != P_RUN) { thread_mark_dead(thread_current_id()); return sched_on_yield(f); }
            RET(r);
        }
        RET(-E_SRCH);
    }
    case 56: {                                                            /* clone(flags, stack, ptid, ctid, tls) */
        if (a0 & 0x10000) RET(proc_clone_thread(p, f, a1, a4, a2, a3, a0));
        RET(proc_fork(p, f, a1));
    }
    case 57: case 58: RET(proc_fork(p, f, 0));                            /* fork, vfork: eager copy of the address space */
    case 435: RET(-E_NOSYS);                                              /* clone3: glibc falls back to clone */
    case 59: {
        if ((r = get_path(p, a0, abs, sizeof abs))) RET(r);
        struct argblock *ab = argblock_new();
        if (!ab) RET(-E_NOMEM);
        if ((r = copy_vec(p, a1, ab, 0)) || (r = copy_vec(p, a2, ab, 1))) { argblock_free(ab); RET(r); }
        const char *err = "";
        if (proc_execve(p, f, abs, ab, &err)) {
            argblock_free(ab);
            RET(!strcmp(err, "no such file") ? -E_NOENT : !strcmp(err, "not a regular file") ? -E_ACCES : -E_NOEXEC);
        }
        argblock_free(ab);
        return f;                                                         /* the frame now describes the new program */
    }
    case 60: return proc_exit_thread(p, f, (int)a0);
    case 231: return proc_exit_group(p, f, (int)a0);
    case 61: RET(proc_wait4(p, (int)(long)(int)a0, a1, (int)a2));
    case 332: RET(-E_NOSYS);                                              /* statx: callers fall back to fstat */
    case 1000: RET(neos_tool_call_ext(p, a0, a1, a2));                    /* NEOS: run a capability-gated tool (same gate as AI agents) */
    default: break;
    }
    if (nr < 512 && t_trace->val && !reported[nr]) { reported[nr] = 1; kprintf("\x1b[93m[linux] unimplemented syscall %lu in '%s' (pid %d)\x1b[0m\n", nr, p->name, p->pid); }
    RET(-E_NOSYS);
}

long neos_tool_call_ext(struct proc *p, u64 cmd, u64 out, u64 outlen) { return (long)neos_tool_call(p, cmd, out, outlen); }

void linux_init(void)
{
    module_register("linux", "1.0", "Linux x86-64 ABI: syscall instruction, fds, pipes, fork/exec/wait, threads, futex, mmap, /proc");
    t_strace  = tune_bool("linux", "strace", "print every Linux syscall with its result (debugging)", TF_LIVE, 0, NULL);
    t_trace   = tune_bool("linux", "trace", "report each unimplemented syscall once", TF_LIVE, 1, NULL);
    t_sysname = tune_str ("linux", "uname_sysname", "what uname -s reports to programs", TF_LIVE, "Linux", NULL);
    t_release = tune_str ("linux", "uname_release", "what uname -r reports to programs", TF_LIVE, "6.1.0-neos", NULL);
    vfs_register_proc_hook(proc_hook);
}
