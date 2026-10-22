#include "proc.h"
#include "agent.h"
#include "fs.h"
#include "shell.h"
#include "varbus.h"
#include "neos_api.h"

static const char *strrchr_(const char *s) { const char *b = s; for (; *s; s++) if (*s == '/') b = s + 1; return b; }

/*
 * Processes: a private lower-half address space, one thread in ring 3, and an identity in the agent
 * subsystem. Anything a program wants from the OS beyond computing and printing (tuning, files,
 * packages, the network...) goes through USYS_TOOL, i.e. through the same capability gate as an AI agent.
 */
static struct proc procs[MAX_PROCS];
static int next_pid = 1;
static struct tunable *t_prio, *t_env_max;

struct proc *proc_current(void) { return sched_current_proc(); }
struct proc *proc_get(int slot) { return slot >= 0 && slot < MAX_PROCS && procs[slot].state != P_FREE ? &procs[slot] : NULL; }
struct proc *proc_by_pid(int pid) { for (int i = 0; i < MAX_PROCS; i++) if (procs[i].state != P_FREE && procs[i].pid == pid) return &procs[i]; return NULL; }

static void die(struct proc *p, i64 code)
{
    u64 fl = irq_save();
    thread_mark_dead(p->tid);
    if (p->state == P_RUN) { p->state = P_ZOMBIE; p->exit_code = code; }
    irq_restore(fl);
}

static void reap_one(struct proc *p)
{
    vm_free(&p->as);
    if (p->principal) { agent_remove(p->principal->name); p->principal = NULL; }
    p->state = P_EXITED;
}

static void reaper(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < MAX_PROCS; i++)
            if (procs[i].state == P_ZOMBIE && thread_is_dead(procs[i].tid)) reap_one(&procs[i]);   /* never while its thread can still run */
        thread_sleep_ms(50);
    }
}

void proc_init(void)
{
    module_register("proc", "1.0", "ring-3 processes, ELF loader, user syscalls (int 0x80), principals in the agent gate");
    t_prio    = tune_int("proc", "prio", "default priority of user processes (0 = highest)", TF_LIVE, 4, 0, 7, NULL);
    t_env_max = tune_int("proc", "env_max", "bytes of environment handed to a new process", TF_LIVE, 3072, 256, 16384, NULL);
    thread_create("reaper", 6, reaper, NULL);
}

/* ------------------------------------------------------------ loading */
static u64 put_str(struct proc *p, u64 *cursor, const char *s)
{
    size_t n = strlen(s) + 1;
    *cursor -= n;
    vm_write(&p->as, *cursor, s, n);
    return *cursor;
}

static int build_stack(struct proc *p, int argc, char **argv, u64 entry, u64 phdr, int phnum, u64 *rsp, const char **err)
{
    #define STACK_PAGES 16
    if (vm_alloc(&p->as, USER_STACK_TOP - STACK_PAGES * 4096, STACK_PAGES, VMF_W)) { *err = "cannot allocate the stack"; return -1; }
    size_t emax = (size_t)t_env_max->val;
    char *env = kmalloc(emax + 64);
    if (!env) { *err = "out of memory"; return -1; }
    int el = bus_env_block(p->world, env, emax);
    if (el < 0 || (size_t)el >= emax) { el = (int)emax - 1; while (el > 0 && env[el - 1] != '\n') el--; }
    el += ksnprintf(env + el, 64, "NEOS_WORLD=%s\nNEOS_PID=%d\n", p->world == NEOS_VIEW_WINDOWS ? "windows" : "linux", p->pid);
    env[el] = 0;

    u64 cur = USER_STACK_TOP, rnd = 0;
    u8 rb[16]; u64 r;
    hw_random(&r) ? (r = rdtsc()) : 0; memcpy(rb, &r, 8); r ^= rdtsc() * 0x9E3779B97F4A7C15ull; memcpy(rb + 8, &r, 8);
    cur -= 16; rnd = cur; vm_write(&p->as, cur, rb, 16);

    u64 *av = kmalloc(sizeof(u64) * (argc + 1)), *ep = kmalloc(sizeof(u64) * 256);
    int nenv = 0;
    if (!av || !ep) { kfree(env); if (av) kfree(av); if (ep) kfree(ep); *err = "out of memory"; return -1; }
    for (int i = argc - 1; i >= 0; i--) av[i] = put_str(p, &cur, argv[i]);
    char *lines[255]; int nl = 0;
    for (char *l = env; *l && nl < 255;) {
        lines[nl++] = l;
        char *e = strchr(l, '\n');
        if (!e) break;
        *e = 0; l = e + 1;
    }
    for (int i = nl - 1; i >= 0; i--) ep[i] = put_str(p, &cur, lines[i]);      /* strings pushed last-to-first, pointers kept in order */
    nenv = nl;
    int words = 1 + argc + 1 + nenv + 1 + 2 * 6 + 2;
    u64 sp = (cur - (u64)words * 8) & ~0xFull;
    u64 *w = kmalloc((size_t)words * 8);
    if (!w) { kfree(env); kfree(av); kfree(ep); *err = "out of memory"; return -1; }
    int k = 0;
    w[k++] = (u64)argc;
    for (int i = 0; i < argc; i++) w[k++] = av[i];
    w[k++] = 0;
    for (int i = 0; i < nenv; i++) w[k++] = ep[i];
    w[k++] = 0;
    w[k++] = 6;  w[k++] = 4096;           /* AT_PAGESZ */
    w[k++] = 9;  w[k++] = entry;          /* AT_ENTRY  */
    w[k++] = 3;  w[k++] = phdr;           /* AT_PHDR   */
    w[k++] = 4;  w[k++] = 56;             /* AT_PHENT  */
    w[k++] = 5;  w[k++] = (u64)phnum;     /* AT_PHNUM  */
    w[k++] = 25; w[k++] = rnd;            /* AT_RANDOM */
    w[k++] = 0;  w[k++] = 0;              /* AT_NULL   */
    int rc = vm_write(&p->as, sp, w, (size_t)k * 8);
    kfree(w); kfree(av); kfree(ep); kfree(env);
    if (rc) { *err = "cannot build the initial stack"; return -1; }
    *rsp = sp;
    return 0;
}

int proc_spawn(const char *path, int argc, char **argv, const struct spawn_opts *o, int *pid_out, const char **err)
{
    const u8 *img; size_t n;
    char full[128];
    fs_norm(path, full, sizeof full);
    if (fs_read(full, &img, &n)) { *err = "no such file"; return -1; }

    struct proc *p = NULL;
    for (int i = 0; i < MAX_PROCS && !p; i++) if (procs[i].state == P_FREE) p = &procs[i];
    for (int i = 0; i < MAX_PROCS && !p; i++) if (procs[i].state == P_EXITED) p = &procs[i];        /* recycle the oldest exited slot */
    if (!p) { *err = "process table full"; return -1; }
    memset(p, 0, sizeof *p);
    p->pid = next_pid++; p->world = o->world ? o->world : NEOS_VIEW_LINUX; p->start_tick = ticks;
    const char *base = strrchr_(full);
    strlcpy(p->name, base, sizeof p->name);

    if (vm_new(&p->as, 0)) { *err = "out of memory"; return -1; }
    u64 phdr = 0, entry = 0, rsp = 0; int phnum = 0;
    if (elf_load(p, img, n, &entry, &phdr, &phnum, err) || build_stack(p, argc, argv, entry, phdr, phnum, &rsp, err)) {
        vm_free(&p->as); p->state = P_FREE; return -1;
    }
    char pname[16]; ksnprintf(pname, sizeof pname, "p%d", p->pid);
    const char *e2 = "";
    p->principal = agent_add(pname, AK_NATIVE, NULL, NULL, 0, &e2);
    if (!p->principal) { vm_free(&p->as); p->state = P_FREE; *err = e2; return -1; }
    p->principal->caps = 1u << CAP_OBSERVE;                      /* a fresh process may look, not touch */
    for (int i = 0; i < o->ngrant; i++) {
        char g[96]; strlcpy(g, o->grant[i], sizeof g);
        char *colon = strchr(g, ':');
        if (colon) *colon = 0;
        if (agent_grant(p->principal, g, colon ? colon + 1 : NULL, &e2)) { agent_remove(pname); vm_free(&p->as); p->state = P_FREE; *err = e2; return -1; }
    }
    p->entry = entry;
    int tid = thread_create_user(p->name, (u8)(o->prio > 0 ? o->prio - 1 : t_prio->val), p, p->as.pml4, entry, rsp);
    if (tid < 0) { agent_remove(pname); vm_free(&p->as); p->state = P_FREE; *err = "no free thread slot"; return -1; }
    p->tid = tid; p->state = P_RUN;
    *pid_out = p->pid;
    return 0;
}

int proc_kill(int pid, i64 code)
{
    struct proc *p = proc_by_pid(pid);
    if (!p || p->state != P_RUN) return -1;
    die(p, code);
    return 0;
}

int proc_wait(int pid, i64 *code, int (*interrupt)(void))
{
    struct proc *p = proc_by_pid(pid);
    if (!p) return -1;
    while (p->state == P_RUN) {
        if (interrupt && interrupt()) return -2;
        thread_sleep_ms(5);
    }
    *code = p->exit_code;
    return 0;
}

/* ------------------------------------------------------------ faults and syscalls */
struct frame *proc_fault(struct frame *f)
{
    struct proc *p = proc_current();
    u64 cr2 = 0;
    if (f->vec == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if (!p) panic("user-mode exception %lu without a process", f->vec);
    kprintf("\n\x1b[91m[proc %d '%s'] killed: %s at rip=%p%s\x1b[0m\n", p->pid, p->name,
            f->vec == 14 ? "page fault" : f->vec == 13 ? "general protection fault" : f->vec == 6 ? "illegal instruction" : f->vec == 0 ? "divide error" : "CPU exception",
            f->rip, f->vec == 14 ? "" : "");
    if (f->vec == 14) kprintf("\x1b[91m          address %p (%s, %s)\x1b[0m\n", cr2, (f->err & 1) ? "protection violation" : "not mapped", (f->err & 16) ? "instruction fetch" : (f->err & 2) ? "write" : "read");
    die(p, f->vec == 0 ? 136 : f->vec == 6 ? 132 : 139);
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
    u64 va = p->mmap_next;
    if (vm_alloc(&p->as, va, pages, VMF_W)) return -NEOS_ENOMEM;
    p->mmap_next += (pages + 1) * 4096;                           /* one unmapped guard page between mappings */
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
    int rc = argc ? agent_call(p->principal, argc, argv, &jb, 0) : 400;
    size_t n = jb.len < outlen - 1 ? jb.len : outlen - 1;
    copy_to_user(&p->as, out, ob, n);
    copy_to_user(&p->as, out + n, "", 1);
    kfree(ob);
    return rc;
}

struct frame *user_syscall(struct frame *f)
{
    struct proc *p = proc_current();
    u64 a = f->rdi, b = f->rsi, c = f->rdx;
    if (!p) panic("user syscall without a process");
    p->syscalls++;
    switch (f->rax) {
    case USYS_EXIT:     die(p, (i64)a); return sched_on_yield(f);
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
