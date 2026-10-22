#include "proc.h"
#include "vfs.h"

/*
 * Linux signal delivery for user processes: handlers run on the thread's own stack with a Linux-compatible rt_sigframe
 * (ucontext + siginfo + FPU state), so glibc / musl / Go return through their own restorer and rt_sigreturn.
 * Delivery happens when a thread is about to return to user mode (after a syscall, an interrupt or a fault).
 */
volatile int sig_any;                                    /* some process has a deliverable pending signal */
static u64 next_alarm;                                   /* earliest alarm deadline in ticks (0 = none armed) */

#define SA_RESTORER 0x04000000ull
#define SA_NODEFER  0x40000000ull
#define SA_RESETHAND 0x80000000ull
#define FXSAVE(p)  __asm__ volatile("fxsave (%0)" :: "r"(p) : "memory")
#define FXRSTOR(p) __asm__ volatile("fxrstor (%0)" :: "r"(p) : "memory")

static inline u64 bit(int sig) { return 1ull << (sig - 1); }

int sig_post(struct proc *p, int sig)
{
    u64 fl = irq_save();
    p->sigpending |= bit(sig);
    sig_any = 1;
    irq_restore(fl);
    return 0;
}

int sig_interrupted(struct proc *p) { return (p->sigpending & ~p->sigmask) != 0; }

/* build the signal frame and redirect the thread into its handler */
static struct frame *deliver(struct proc *p, struct frame *f, int sig, u64 fault_addr, int code)
{
    struct sigact *a = &p->sig[sig];
    if (!(a->flags & SA_RESTORER) || !a->restorer) goto kill;
    u64 fp = (f->rsp - 128 - 512) & ~15ull, info = fp - 128, uc = (info - 448) & ~15ull, sp = uc - 8;
    u8 kuc[448]; u8 kinfo[128]; u8 kfx[512] __attribute__((aligned(16)));
    memset(kuc, 0, sizeof kuc); memset(kinfo, 0, sizeof kinfo);
    u64 *g = (u64 *)(kuc + 40);                                           /* uc_mcontext.gregs */
    g[0] = f->r8; g[1] = f->r9; g[2] = f->r10; g[3] = f->r11; g[4] = f->r12; g[5] = f->r13; g[6] = f->r14; g[7] = f->r15;
    g[8] = f->rdi; g[9] = f->rsi; g[10] = f->rbp; g[11] = f->rbx; g[12] = f->rdx; g[13] = f->rax; g[14] = f->rcx; g[15] = f->rsp;
    g[16] = f->rip; g[17] = f->rflags; g[18] = 0x33; g[19] = f->err; g[20] = f->vec; g[21] = 0; g[22] = fault_addr;
    *(u64 *)(kuc + 40 + 184) = fp;                                        /* fpregs pointer */
    *(u64 *)(kuc + 40 + 256) = p->susp ? p->susp_mask : p->sigmask;      /* uc_sigmask: restored by rt_sigreturn */
    p->susp = 0;
    *(u32 *)(kuc + 16 + 8) = 2;                                           /* uc_stack.ss_flags = SS_DISABLE */
    *(int *)kinfo = sig; *(int *)(kinfo + 8) = code; *(u64 *)(kinfo + 16) = fault_addr;
    FXSAVE(kfx);
    if (copy_to_user(&p->as, sp, &a->restorer, 8) || copy_to_user(&p->as, uc, kuc, sizeof kuc) ||
        copy_to_user(&p->as, info, kinfo, sizeof kinfo) || copy_to_user(&p->as, fp, kfx, 512)) goto kill;
    p->sigmask |= a->mask | ((a->flags & SA_NODEFER) ? 0 : bit(sig));
    if (a->flags & SA_RESETHAND) a->handler = 0;
    f->rip = a->handler; f->rsp = sp; f->rdi = (u64)sig; f->rsi = info; f->rdx = uc; f->rax = 0;
    f->rflags = (f->rflags & ~0x400ull) | 0x202;
    return f;
kill:
    kprintf("\n\x1b[91m[proc %d '%s'] killed: cannot deliver signal %d\x1b[0m\n", p->pid, p->name, sig);
    extern void proc_kill_signal(struct proc *p, int sig);
    proc_kill_signal(p, 11);
    thread_mark_dead(thread_current_id());
    return sched_on_yield(f);
}

struct frame *signal_deliver(struct frame *f)
{
    struct proc *p = proc_current();
    if (!p || p->state != P_RUN) return f;
    u64 ok = p->sigpending & ~p->sigmask;
    if (!ok) { if (!(p->sigpending)) { /* nothing pending anywhere cheap to know: leave sig_any for other processes */ } return f; }
    int sig = __builtin_ctzll(ok) + 1;
    p->sigpending &= ~bit(sig);
    if (p->sig[sig].handler <= 1) return f;
    return deliver(p, f, sig, 0, 0);
}

/* synchronous fault (SIGSEGV/SIGILL/SIGFPE...) with a handler installed: run it instead of killing the process */
struct frame *signal_fault(struct frame *f, int sig, u64 addr, int code)
{
    struct proc *p = proc_current();
    if (!p || p->sig[sig].handler <= 1 || (p->sigmask & bit(sig))) return NULL;
    return deliver(p, f, sig, addr, code);
}

long sig_return(struct proc *p, struct frame *f)
{
    u64 uc = f->rsp;
    u8 kuc[448];
    if (copy_from_user(&p->as, kuc, uc, sizeof kuc)) return -E_FAULT;
    u64 *g = (u64 *)(kuc + 40);
    if (g[16] >= USER_TOP || g[15] >= USER_TOP) return -E_FAULT;
    f->r8 = g[0]; f->r9 = g[1]; f->r10 = g[2]; f->r11 = g[3]; f->r12 = g[4]; f->r13 = g[5]; f->r14 = g[6]; f->r15 = g[7];
    f->rdi = g[8]; f->rsi = g[9]; f->rbp = g[10]; f->rbx = g[11]; f->rdx = g[12]; f->rax = g[13]; f->rcx = g[14]; f->rsp = g[15];
    f->rip = g[16]; f->rflags = (g[17] & 0x8D5) | 0x202;
    p->sigmask = *(u64 *)(kuc + 40 + 256) & ~(bit(9) | bit(19));
    u64 fpp = *(u64 *)(kuc + 40 + 184);
    if (fpp) { u8 kfx[512] __attribute__((aligned(16))); if (!copy_from_user(&p->as, kfx, fpp, 512)) { *(u32 *)(kfx + 24) &= 0x0000FFBF; FXRSTOR(kfx); } }
    return 0;
}

/* ---- timers: alarm(2) / setitimer(ITIMER_REAL) ---- */
void alarm_set(struct proc *p, u64 deadline, u64 interval)
{
    p->alarm_at = deadline; p->alarm_int = interval;
    if (deadline && (!next_alarm || deadline < next_alarm)) next_alarm = deadline;
}

void alarm_tick(void)
{
    if (!next_alarm || ticks < next_alarm) return;
    next_alarm = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_get(i);
        if (!p || p->state != P_RUN || !p->alarm_at) continue;
        if (ticks >= p->alarm_at) {
            p->alarm_at = p->alarm_int ? ticks + p->alarm_int : 0;
            extern int proc_signal(int pid, int sig);
            proc_signal(p->pid, 14);
        }
        if (p->alarm_at && (!next_alarm || p->alarm_at < next_alarm)) next_alarm = p->alarm_at;
    }
}
