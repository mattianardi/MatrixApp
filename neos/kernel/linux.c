#include "proc.h"
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
    struct file *f = file_new(dir ? FT_DIR : i->dev == DEV_TTY ? FT_TTY : FT_INODE, flags);
    if (!f) return -E_NOMEM;
    f->ino = i;
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
        if ((size_t)r < c || f->type == FT_TTY || f->type == FT_PIPE_R) break;      /* short read / interactive: return what we have */
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
        char name[64]; int type; u32 ino;
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
static u8 fkeys[512];
static void *fkey(struct proc *p, u64 addr) { return &fkeys[((p->as.pml4 >> 12) * 31 + (addr >> 2)) & 511]; }

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
    case 5: return -E_NOSYS;
    }
    return -E_NOSYS;
}

/* ------------------------------------------------------------ misc */
static long sys_nanosleep(struct proc *p, u64 ureq, u64 urem)
{
    u64 ts[2];
    if (copy_from_user(&p->as, ts, ureq, 16)) return -E_FAULT;
    u64 ns = ts[0] * 1000000000ull + ts[1];
    thread_sleep_ms((ns + 999999) / 1000000);
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
        extern int tty_pump(void);
        tty_pump();
        thread_sleep_ms(2);
    }
}

static long sys_ioctl(struct proc *p, int fd, u64 req, u64 arg)
{
    struct file *f = fdt_get(p->fds, fd);
    if (!f) return -E_BADF;
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
    case 271: { long to = -1; if (a2) { u64 ts[2]; if (copy_from_user(&p->as, ts, a2, 16)) RET(-E_FAULT); to = (long)(ts[0] * 1000 + ts[1] / 1000000); } RET(sys_poll(p, a0, a1, to)); }
    case 9:  RET(sys_mmap(p, a0, a1, (int)a2, (int)a3, (long)(int)a4, a5));
    case 10: RET(vm_mprotect(&p->as, a0, a1, (int)a2) ? -E_NOMEM : 0);
    case 11: RET(vm_munmap(&p->as, a0, a1) ? -E_INVAL : 0);
    case 12: RET(sys_brk(p, a0));
    case 25: RET(-E_NOMEM);                                               /* mremap: callers fall back to mmap+copy */
    case 28: case 149: case 150: case 27: RET(0);                         /* madvise, mlock, munlock, mincore */
    case 13: {                                                            /* rt_sigaction */
        if (a0 < 1 || a0 > 64 || a0 == 9 || a0 == 19) RET(-E_INVAL);
        if (a2) { u64 o[4] = { p->sig[a0].handler, p->sig[a0].flags, p->sig[a0].restorer, p->sig[a0].mask }; if (copy_to_user(&p->as, a2, o, 32)) RET(-E_FAULT); }
        if (a1) { u64 nw[4]; if (copy_from_user(&p->as, nw, a1, 32)) RET(-E_FAULT); p->sig[a0] = (struct sigact){ nw[0], nw[1], nw[3], nw[2] }; }
        RET(0);
    }
    case 14: { if (a2) { u64 z = 0; copy_to_user(&p->as, a2, &z, 8); } RET(0); }       /* rt_sigprocmask */
    case 131: case 130: case 127: RET(0);                                 /* sigaltstack, sigsuspend, sigpending */
    case 15: RET(-E_INVAL);                                               /* rt_sigreturn: handlers are not delivered yet */
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
    case 200: case 234: if (a1 == 0 || (nr == 234 && a2 == 0)) RET(0); RET(0);                  /* thread-directed signals are not delivered yet */
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
