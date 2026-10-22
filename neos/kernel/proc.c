#include "proc.h"
#include "agent.h"
#include "fs.h"
#include "shell.h"
#include "varbus.h"
#include "neos_api.h"

/*
 * Processes: a private lower-half address space, threads in ring 3, open files, and an identity (principal) in the
 * agent subsystem. Anything a program wants from the OS beyond computing and I/O (tuning, packages, the network...)
 * goes through the capability gate - from the native int 0x80 ABI or from the Linux ABI alike.
 */
static struct proc procs[MAX_PROCS];
static int next_pid = 1;
static struct tunable *t_prio, *t_env_max;
#define STACK_SIZE (8ull << 20)
#define MMAP_BASE  0x00007E0000000000ull
#define INTERP_BASE 0x00007FF000000000ull

static const char *base_name(const char *s) { const char *b = s; for (; *s; s++) if (*s == '/') b = s + 1; return b; }

struct proc *proc_current(void) { return sched_current_proc(); }
struct proc *proc_get(int slot) { return slot >= 0 && slot < MAX_PROCS && procs[slot].state != P_FREE ? &procs[slot] : NULL; }
struct proc *proc_by_pid(int pid) { for (int i = 0; i < MAX_PROCS; i++) if (procs[i].state != P_FREE && procs[i].pid == pid) return &procs[i]; return NULL; }

/* ------------------------------------------------------------ lifecycle */
static void finish(struct proc *p, int wstatus, i64 shell_code)
{
    u64 fl = irq_save();
    if (p->state != P_RUN) { irq_restore(fl); return; }
    p->state = P_ZOMBIE; p->exit_status = wstatus; p->exit_code = shell_code;
    /* Other threads can stop now; the CURRENT one must finish the cleanup below (closing files wakes pipe readers),
     * so the caller marks it dead only at the very end - otherwise a timer tick here would strand the cleanup. */
    sched_kill_threads_of_except(p, thread_current_id());
    for (int i = 0; i < MAX_PROCS; i++) if (procs[i].state != P_FREE && procs[i].ppid == p->pid) {       /* orphans go to the shell */
        procs[i].ppid = 0;
        if (procs[i].state >= P_ZOMBIE) procs[i].collected = 1;
    }
    struct fdtable *fds = p->fds; p->fds = NULL;
    struct proc *par = p->ppid ? proc_by_pid(p->ppid) : NULL;
    irq_restore(fl);
    if (fds) fdt_release(fds);
    if (par) wake_on(par, 16);
    wake_on(&procs, 16);                                        /* shell-side proc_wait */
}

static void reap_one(struct proc *p)
{
    vm_free(&p->as);
    if (p->principal) { agent_remove(p->principal->name); p->principal = NULL; }
    p->state = P_EXITED;
    if (p->ppid == 0) p->collected = 0;
}

static void reaper(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < MAX_PROCS; i++)
            if (procs[i].state == P_ZOMBIE && !sched_threads_alive(&procs[i])) reap_one(&procs[i]);    /* never while a thread can still run */
        thread_sleep_ms(50);
    }
}

void proc_init(void)
{
    module_register("proc", "2.0", "ring-3 processes: fork/exec/threads, ELF loader, native + Linux ABI, principals in the agent gate");
    t_prio    = tune_int("proc", "prio", "default priority of user processes (0 = highest)", TF_LIVE, 4, 0, 7, NULL);
    t_env_max = tune_int("proc", "env_max", "bytes of environment handed to a process started by the shell", TF_LIVE, 3072, 256, 16384, NULL);
    thread_create("reaper", 6, reaper, NULL);
}

static struct proc *alloc_proc(void)
{
    u64 fl = irq_save();                                       /* the slot is claimed before anything can preempt us */
    struct proc *p = NULL;
    for (int i = 0; i < MAX_PROCS && !p; i++) if (procs[i].state == P_FREE) p = &procs[i];
    for (int i = 0; i < MAX_PROCS && !p; i++) if (procs[i].state == P_EXITED && (procs[i].collected || procs[i].ppid == 0)) p = &procs[i];
    for (int i = 0; i < MAX_PROCS && !p; i++) if (procs[i].state == P_EXITED) p = &procs[i];
    if (p) { memset(p, 0, sizeof *p); p->pid = next_pid++; p->state = P_BUILD; }
    irq_restore(fl);
    return p;
}

static void abandon_proc(struct proc *p) { p->state = P_FREE; }

/* ------------------------------------------------------------ argv / envp blocks */
struct argblock *argblock_new(void)
{
    struct argblock *a = kmalloc(sizeof *a);
    if (!a) return NULL;
    a->cap = 65536; a->used = 0; a->argc = a->envc = 0;
    a->pool = kmalloc(a->cap); a->argv = kmalloc(sizeof(char *) * 258); a->envp = kmalloc(sizeof(char *) * 258);
    if (!a->pool || !a->argv || !a->envp) { argblock_free(a); return NULL; }
    return a;
}
void argblock_free(struct argblock *a)
{
    if (!a) return;
    if (a->pool) kfree(a->pool);
    if (a->argv) kfree(a->argv);
    if (a->envp) kfree(a->envp);
    kfree(a);
}
int argblock_add(struct argblock *a, int env, const char *s)
{
    size_t n = strlen(s) + 1;
    if (a->used + n > a->cap || (env ? a->envc : a->argc) >= 256) return -1;
    char *d = a->pool + a->used;
    memcpy(d, s, n); a->used += n;
    if (env) a->envp[a->envc++] = d; else a->argv[a->argc++] = d;
    return 0;
}

/* ------------------------------------------------------------ building the initial stack (System V / Linux layout) */
static u64 put_str(struct aspace *as, u64 *cur, const char *s)
{
    size_t n = strlen(s) + 1;
    *cur -= n;
    vm_write(as, *cur, s, n);
    return *cur;
}

static int build_stack(struct aspace *as, struct argblock *ab, const char *execfn, const struct elf_info *mi, const struct elf_info *ii, u64 *rsp, const char **err)
{
    if (vma_add(as, USER_STACK_TOP - STACK_SIZE, USER_STACK_TOP, PROT_READ | PROT_WRITE, NULL, 0)) { *err = "too many mappings"; return -1; }
    u64 cur = USER_STACK_TOP, rnd;
    u8 rb[16]; fill_random(rb, 16);
    cur -= 16; rnd = cur; vm_write(as, cur, rb, 16);
    u64 fn = put_str(as, &cur, execfn), plat = put_str(as, &cur, "x86_64");
    u64 *ea = kmalloc(sizeof(u64) * 258), *aa = kmalloc(sizeof(u64) * 258);
    if (!ea || !aa) { if (ea) kfree(ea); if (aa) kfree(aa); *err = "out of memory"; return -1; }
    for (int i = ab->envc - 1; i >= 0; i--) ea[i] = put_str(as, &cur, ab->envp[i]);
    for (int i = ab->argc - 1; i >= 0; i--) aa[i] = put_str(as, &cur, ab->argv[i]);
    u32 a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int words = 1 + ab->argc + 1 + ab->envc + 1 + 2 * 24 + 2;     /* room for all auxv pairs */
    u64 sp = (cur - (u64)words * 8) & ~0xFull;
    u64 *w = kmalloc((size_t)words * 8);
    if (!w) { kfree(ea); kfree(aa); *err = "out of memory"; return -1; }
    int k = 0;
    w[k++] = (u64)ab->argc;
    for (int i = 0; i < ab->argc; i++) w[k++] = aa[i];
    w[k++] = 0;
    for (int i = 0; i < ab->envc; i++) w[k++] = ea[i];
    w[k++] = 0;
    #define AUX(t, v) do { w[k++] = (t); w[k++] = (v); } while (0)
    AUX(16, d); AUX(6, 4096); AUX(17, 100); AUX(3, mi->phdr); AUX(4, 56); AUX(5, (u64)mi->phnum);
    AUX(7, ii ? ii->base : 0); AUX(8, 0); AUX(9, mi->entry); AUX(11, 0); AUX(12, 0); AUX(13, 0); AUX(14, 0);
    AUX(15, plat); AUX(23, 0); AUX(25, rnd); AUX(31, fn);
    AUX(0, 0);
    int rc = vm_write(as, sp, w, (size_t)k * 8);
    kfree(w); kfree(ea); kfree(aa);
    if (rc) { *err = "cannot build the initial stack"; return -1; }
    *rsp = sp;
    return 0;
}

/* ------------------------------------------------------------ program loading: shebang, ELF, dynamic linker */
static int load_image(struct proc *p, struct aspace *as, const char *path, struct argblock *ab, int depth,
                      u64 *entry, u64 *sp, u64 *brk_start, const char **err)
{
    const u8 *img; size_t n;
    char full[128];
    if (!strcmp(path, "/proc/self/exe")) strlcpy(full, p->exe, sizeof full);
    else fs_norm_at(p->cwd[0] ? p->cwd : "/", path, full, sizeof full);
    struct inode *ino = vfs_lookup(full);
    if (!ino) { *err = "no such file"; return -1; }
    if ((ino->mode & S_IFMT) != S_IFREG) { *err = "not a regular file"; return -1; }
    img = ino->data; n = ino->size;

    if (n >= 3 && img[0] == '#' && img[1] == '!') {                         /* #!interpreter [arg] */
        if (depth >= 4) { *err = "too many nested interpreters"; return -1; }
        char line[160]; size_t l = 0;
        for (size_t i = 2; i < n && i < 2 + sizeof line - 1 && img[i] != '\n'; i++) line[l++] = (char)img[i];
        line[l] = 0;
        char *ip = line; while (*ip == ' ') ip++;
        char *arg = strchr(ip, ' '); if (arg) { *arg++ = 0; while (*arg == ' ') arg++; if (!*arg) arg = NULL; }
        struct argblock *nb = argblock_new();
        if (!nb) { *err = "out of memory"; return -1; }
        argblock_add(nb, 0, ip);
        if (arg) argblock_add(nb, 0, arg);
        argblock_add(nb, 0, full);
        for (int i = 1; i < ab->argc; i++) argblock_add(nb, 0, ab->argv[i]);
        for (int i = 0; i < ab->envc; i++) argblock_add(nb, 1, ab->envp[i]);
        int r = load_image(p, as, ip, nb, depth + 1, entry, sp, brk_start, err);
        argblock_free(nb);
        return r;
    }

    struct elf_info mi, ii; int have_interp = 0;
    if (elf_load(as, img, n, 0, &mi, err)) return -1;
    u64 start = mi.entry;
    if (mi.interp[0]) {                                                     /* dynamically linked: the dynamic linker runs first */
        char ip[128]; fs_norm_at("/", mi.interp, ip, sizeof ip);
        struct inode *ii_ino = vfs_lookup(ip);
        if (!ii_ino || !ii_ino->data) { *err = "dynamic linker not found in the filesystem"; return -1; }
        if (elf_load(as, ii_ino->data, ii_ino->size, INTERP_BASE, &ii, err)) return -1;
        start = ii.entry; have_interp = 1;
    }
    if (build_stack(as, ab, full, &mi, have_interp ? &ii : NULL, sp, err)) return -1;
    *entry = start;
    *brk_start = (mi.top + 0xFFF) & ~0xFFFull;
    strlcpy(p->exe, full, sizeof p->exe);
    return 0;
}

/* ------------------------------------------------------------ spawn (the shell's `run`) */
static void setup_stdio(struct proc *p)
{
    struct file *tty = file_new(FT_TTY, O_RDWR);
    fdt_install(p->fds, tty, 0, 0);
    file_get(tty); fdt_install(p->fds, tty, 1, 0);
    file_get(tty); fdt_install(p->fds, tty, 2, 0);
}

int proc_spawn(const char *path, int argc, char **argv, const struct spawn_opts *o, int *pid_out, const char **err)
{
    struct proc *p = alloc_proc();
    if (!p) { *err = "process table full"; return -1; }
    p->world = o->world ? o->world : NEOS_VIEW_LINUX;
    p->start_tick = ticks;
    bus_get("PWD", NEOS_VIEW_LINUX, p->cwd, sizeof p->cwd);
    if (!p->cwd[0]) strlcpy(p->cwd, "/", sizeof p->cwd);
    p->fds = fdt_new();
    if (!p->fds) { abandon_proc(p); *err = "out of memory"; return -1; }
    setup_stdio(p);

    struct argblock *ab = argblock_new();
    if (!ab) { fdt_release(p->fds); abandon_proc(p); *err = "out of memory"; return -1; }
    for (int i = 0; i < argc; i++) argblock_add(ab, 0, argv[i]);
    size_t emax = (size_t)t_env_max->val;
    char *env = kmalloc(emax + 64);
    if (env) {
        int el = bus_env_block(p->world, env, emax);
        if (el < 0 || (size_t)el >= emax) { el = (int)emax - 1; while (el > 0 && env[el - 1] != '\n') el--; }
        el += ksnprintf(env + el, 64, "NEOS_WORLD=%s\nNEOS_PID=%d\n", p->world == NEOS_VIEW_WINDOWS ? "windows" : "linux", p->pid);
        env[el] = 0;
        for (char *l = env; *l;) { char *e = strchr(l, '\n'); if (e) *e = 0; argblock_add(ab, 1, l); if (!e) break; l = e + 1; }
        kfree(env);
    }
    if (vm_new(&p->as, 0)) { argblock_free(ab); fdt_release(p->fds); abandon_proc(p); *err = "out of memory"; return -1; }
    u64 entry = 0, sp = 0, brk0 = 0;
    if (load_image(p, &p->as, path, ab, 0, &entry, &sp, &brk0, err)) { argblock_free(ab); vm_free(&p->as); fdt_release(p->fds); abandon_proc(p); return -1; }
    argblock_free(ab);
    strlcpy(p->name, base_name(p->exe), sizeof p->name);
    p->entry = entry; p->brk_start = p->brk = brk0; p->mmap_next = MMAP_BASE;

    char pname[16]; ksnprintf(pname, sizeof pname, "p%d", p->pid);
    const char *e2 = "";
    p->principal = agent_add(pname, AK_NATIVE, NULL, NULL, 0, &e2);
    if (!p->principal) { vm_free(&p->as); fdt_release(p->fds); abandon_proc(p); *err = e2; return -1; }
    p->principal->caps = 1u << CAP_OBSERVE;                      /* a fresh process may look, not touch */
    for (int i = 0; i < o->ngrant; i++) {
        char g[96]; strlcpy(g, o->grant[i], sizeof g);
        char *colon = strchr(g, ':');
        if (colon) *colon = 0;
        if (agent_grant(p->principal, g, colon ? colon + 1 : NULL, &e2)) { agent_remove(pname); vm_free(&p->as); fdt_release(p->fds); abandon_proc(p); *err = e2; return -1; }
    }
    p->nthreads = 1; p->next_utid = p->pid + 1;
    struct uctx uc = { .fsbase = 0, .clear_tid = 0, .utid = p->pid };
    p->state = P_RUN;                                          /* before the thread exists: it may exit immediately */
    int tid = thread_create_user(p->name, (u8)(o->prio > 0 ? o->prio - 1 : t_prio->val), p, p->as.pml4, entry, sp, &uc);
    if (tid < 0) { p->state = P_BUILD; agent_remove(pname); vm_free(&p->as); fdt_release(p->fds); abandon_proc(p); *err = "no free thread slot"; return -1; }
    p->main_tid = tid;
    *pid_out = p->pid;
    return 0;
}

/* ------------------------------------------------------------ execve / fork / clone */
int proc_execve(struct proc *p, struct frame *f, const char *path, struct argblock *ab, const char **err)
{
    struct aspace *na = kmalloc(sizeof *na);                   /* per call: execs of different processes can interleave */
    u64 entry, sp, brk0;
    if (!na) { *err = "out of memory"; return -1; }
    if (vm_new(na, p->as.limit)) { kfree(na); *err = "out of memory"; return -1; }
    if (load_image(p, na, path, ab, 0, &entry, &sp, &brk0, err)) { vm_free(na); kfree(na); return -1; }
    /* point of no return */
    sched_kill_threads_of_except(p, thread_current_id());
    p->nthreads = 1;
    struct aspace *old = kmalloc(sizeof *old);
    if (old) *old = p->as;
    p->as = *na;
    kfree(na);
    sched_switch_cr3(p->as.pml4);
    if (old) { vm_free(old); kfree(old); }
    fdt_close_cloexec(p->fds);
    memset(p->sig, 0, sizeof p->sig);
    sched_set_fsbase(0);
    uctx_current()->clear_tid = 0;
    p->entry = entry; p->brk_start = p->brk = brk0; p->mmap_next = MMAP_BASE;
    strlcpy(p->name, base_name(p->exe), sizeof p->name);
    memset(f, 0, offsetof(struct frame, vec));                 /* clear every general register */
    f->rip = entry; f->rsp = sp; f->rflags = 0x202;
    return 0;
}

static int copy_principal(struct proc *c, struct proc *p)
{
    char pname[16]; const char *e = "";
    ksnprintf(pname, sizeof pname, "p%d", c->pid);
    c->principal = agent_add(pname, AK_NATIVE, NULL, NULL, 0, &e);
    if (!c->principal) return -1;
    if (p->principal) {
        c->principal->caps = p->principal->caps;
        memcpy(c->principal->scope, p->principal->scope, sizeof c->principal->scope);
        memcpy(c->principal->policy, p->principal->policy, sizeof c->principal->policy);
    } else c->principal->caps = 1u << CAP_OBSERVE;
    return 0;
}

long proc_fork(struct proc *p, const struct frame *f, u64 child_stack)
{
    struct proc *c = alloc_proc();
    if (!c) return -E_AGAIN;
    c->ppid = p->pid; c->world = p->world; c->start_tick = ticks;
    strlcpy(c->name, p->name, sizeof c->name); strlcpy(c->exe, p->exe, sizeof c->exe); strlcpy(c->cwd, p->cwd, sizeof c->cwd);
    c->brk_start = p->brk_start; c->brk = p->brk; c->mmap_next = p->mmap_next; c->entry = p->entry;
    memcpy(c->sig, p->sig, sizeof c->sig);
    if (vm_new(&c->as, p->as.limit)) { abandon_proc(c); return -E_NOMEM; }
    if (vm_clone(&c->as, &p->as)) { vm_free(&c->as); abandon_proc(c); return -E_NOMEM; }
    c->fds = fdt_clone(p->fds);
    if (!c->fds) { vm_free(&c->as); abandon_proc(c); return -E_NOMEM; }
    if (copy_principal(c, p)) { fdt_release(c->fds); vm_free(&c->as); abandon_proc(c); return -E_NOMEM; }
    struct frame cf = *f;
    cf.rax = 0;
    if (child_stack) cf.rsp = child_stack;
    c->nthreads = 1; c->next_utid = c->pid + 1; c->state = P_RUN;
    struct uctx cu = { .fsbase = uctx_current()->fsbase, .clear_tid = 0, .utid = c->pid };
    int tid = thread_create_user_frame(c->name, (u8)thread_prio_of(thread_current_id()), c, c->as.pml4, &cf, &cu);
    if (tid < 0) { agent_remove(c->principal->name); fdt_release(c->fds); vm_free(&c->as); abandon_proc(c); return -E_AGAIN; }
    c->main_tid = tid;
    return c->pid;
}

#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

long proc_clone_thread(struct proc *p, const struct frame *f, u64 child_stack, u64 tls, u64 ptid, u64 ctid, u64 flags)
{
    struct frame cf = *f;
    cf.rax = 0;
    if (child_stack) cf.rsp = child_stack;
    int utid = p->next_utid++;
    if (utid < next_pid) { utid = next_pid++; }
    struct uctx nu = { .fsbase = (flags & CLONE_SETTLS) ? tls : uctx_current()->fsbase, .clear_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0, .utid = utid };
    p->nthreads++;
    int tid = thread_create_user_frame(p->name, (u8)thread_prio_of(thread_current_id()), p, p->as.pml4, &cf, &nu);
    if (tid < 0) { p->nthreads--; return -E_AGAIN; }
    u32 v = (u32)utid;
    if ((flags & CLONE_PARENT_SETTID) && ptid) copy_to_user(&p->as, ptid, &v, 4);
    if ((flags & CLONE_CHILD_SETTID) && ctid) copy_to_user(&p->as, ctid, &v, 4);
    return utid;
}

struct frame *proc_exit_thread(struct proc *p, struct frame *f, int status)
{
    struct uctx *u = uctx_current();
    if (u->clear_tid) {
        u32 z = 0;
        copy_to_user(&p->as, u->clear_tid, &z, 4);
        extern void futex_wake_addr(struct proc *p, u64 addr, int n);
        futex_wake_addr(p, u->clear_tid, 1);
    }
    if (--p->nthreads <= 0) finish(p, (status & 0xFF) << 8, status & 0xFF);
    thread_mark_dead(thread_current_id());
    return sched_on_yield(f);
}

struct frame *proc_exit_group(struct proc *p, struct frame *f, int status)
{
    finish(p, (status & 0xFF) << 8, status & 0xFF);
    thread_mark_dead(thread_current_id());
    return sched_on_yield(f);
}

static void kill_with_signal(struct proc *p, int sig) { finish(p, sig & 0x7F, 128 + sig); }

int proc_signal(int pid, int sig)
{
    struct proc *p = proc_by_pid(pid);
    if (!p || p->state != P_RUN) return -E_SRCH;
    if (sig < 0 || sig > 64) return -E_INVAL;
    if (sig == 0) return 0;
    u64 h = p->sig[sig].handler;
    if (sig != 9 && h == 1) return 0;                                      /* SIG_IGN */
    switch (sig) {
    case 17: case 18: case 20: case 21: case 22: case 23: case 28: return 0;     /* ignored / job control by default */
    }
    if (sig != 9 && h > 1) return 0;                                       /* a handler is installed: delivery of handlers is not implemented yet */
    kill_with_signal(p, sig);
    return 0;
}

int proc_kill(int pid, i64 code)
{
    struct proc *p = proc_by_pid(pid);
    if (!p || p->state != P_RUN) return -1;
    finish(p, (int)(code & 0x7F), code);
    return 0;
}

int proc_wait(int pid, i64 *code, int (*interrupt)(void))
{
    struct proc *p = proc_by_pid(pid);
    if (!p) return -1;
    while (p->state == P_RUN) {
        if (interrupt && interrupt()) return -2;
        u64 fl = irq_save();
        if (p->state == P_RUN) { sleep_on_locked(&procs, ticks + ms_to_ticks(20)); }
        irq_restore(fl);
    }
    *code = p->exit_code;
    p->collected = 1;
    return 0;
}

long proc_wait4(struct proc *p, int pid, u64 ustatus, int options)
{
    for (;;) {
        int kids = 0;
        u64 fl = irq_save();
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *c = &procs[i];
            if (c->state == P_FREE || c->ppid != p->pid || c->collected) continue;
            if (pid > 0 && c->pid != pid) continue;
            kids++;
            if (c->state >= P_ZOMBIE) {
                c->collected = 1;
                int st = c->exit_status, cp = c->pid;
                irq_restore(fl);
                if (ustatus) copy_to_user(&p->as, ustatus, &st, 4);
                return cp;
            }
        }
        if (!kids) { irq_restore(fl); return -E_CHILD; }
        if (options & 1) { irq_restore(fl); return 0; }                    /* WNOHANG */
        sleep_on_locked(p, ticks + ms_to_ticks(200));
        irq_restore(fl);
    }
}

/* ------------------------------------------------------------ faults and native syscalls */
struct frame *proc_fault(struct frame *f)
{
    struct proc *p = proc_current();
    u64 cr2 = 0;
    if (f->vec == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if (!p) panic("user-mode exception %lu without a process", f->vec);
    if (f->vec == 14 && !(f->err & 1) && !vm_fault(&p->as, cr2, (f->err & 2) != 0, (f->err & 16) != 0))
        return f;                                                          /* demand paging: the access is retried */
    kprintf("\n\x1b[91m[proc %d '%s'] killed: %s at rip=%p\x1b[0m\n", p->pid, p->name,
            f->vec == 14 ? "page fault" : f->vec == 13 ? "general protection fault" : f->vec == 6 ? "illegal instruction" : f->vec == 0 ? "divide error" : "CPU exception", f->rip);
    if (f->vec == 14) kprintf("\x1b[91m          address %p (%s, %s)\x1b[0m\n", cr2, (f->err & 1) ? "protection violation" : "not mapped", (f->err & 16) ? "instruction fetch" : (f->err & 2) ? "write" : "read");
    int sig = f->vec == 0 ? 8 : f->vec == 6 ? 4 : 11;
    kill_with_signal(p, sig);
    thread_mark_dead(thread_current_id());
    return sched_on_yield(f);
}

static i64 sys_write(struct proc *p, u64 fd, u64 buf, u64 len)
{
    if (fd != 1 && fd != 2) return -9;
    if (len > 4096) len = 4096;
    char tmp[256];
    for (u64 done = 0; done < len;) {
        u64 c = len - done < sizeof tmp ? len - done : sizeof tmp;
        if (copy_from_user(&p->as, tmp, buf + done, c)) return done ? (i64)done : -NEOS_EFAULT;
        for (u64 i = 0; i < c; i++) kputc(tmp[i]);
        done += c;
    }
    return (i64)len;
}

static i64 sys_mmap(struct proc *p, u64 len)
{
    u64 pages = (len + 4095) / 4096;
    if (!pages || pages > 1024) return -NEOS_ENOMEM;
    u64 va = vm_find_gap(&p->as, p->mmap_next, pages * 4096);
    if (!va || vm_alloc(&p->as, va, pages, VMF_W)) return -NEOS_ENOMEM;
    p->mmap_next = va + (pages + 1) * 4096;
    return (i64)va;
}

static i64 sys_tool(struct proc *p, u64 cmd, u64 out, u64 outlen)
{
    char line[512], *argv[12];
    if (strncpy_from_user(&p->as, line, cmd, sizeof line) < 0) return -NEOS_EFAULT;
    if (outlen == 0 || outlen > 8192 || !user_range_ok(&p->as, out, outlen, 1)) return -NEOS_EFAULT;
    char *ob = kmalloc(4096);
    if (!ob) return -NEOS_ENOMEM;
    struct jbuf jb = { ob, 4096, 0 };
    int argc = shell_tokenize(line, argv, 12);
    int rc = argc && p->principal ? agent_call(p->principal, argc, argv, &jb, 0) : 400;
    size_t n = jb.len < outlen - 1 ? jb.len : outlen - 1;
    copy_to_user(&p->as, out, ob, n);
    copy_to_user(&p->as, out + n, "", 1);
    kfree(ob);
    return rc;
}

i64 neos_tool_call(struct proc *p, u64 cmd, u64 out, u64 outlen) { return sys_tool(p, cmd, out, outlen); }

struct frame *user_syscall(struct frame *f)
{
    struct proc *p = proc_current();
    u64 a = f->rdi, b = f->rsi, c = f->rdx;
    if (!p) panic("user syscall without a process");
    p->syscalls++;
    switch (f->rax) {
    case USYS_EXIT:     return proc_exit_group(p, f, (int)a);
    case USYS_WRITE:    f->rax = (u64)sys_write(p, a, b, c); break;
    case USYS_GETPID:   f->rax = (u64)p->pid; break;
    case USYS_YIELD:    thread_yield(); f->rax = 0; break;
    case USYS_SLEEP_MS: thread_sleep_ms(a > 600000 ? 600000 : a); f->rax = 0; break;
    case USYS_TIME_NS:  f->rax = time_ns(); break;
    case USYS_MMAP:     f->rax = (u64)sys_mmap(p, a); break;
    case USYS_TOOL:     f->rax = (u64)sys_tool(p, a, b, c); break;
    default:            f->rax = (u64)-NEOS_ENOSYS; break;
    }
    return f;
}
