#include "kernel.h"
#include "neos_api.h"
#include "vm.h"

/*
 * Deterministic scheduler: fixed priorities, no allocation and no priority
 * recalculation in the dispatch path, bounded O(SCHED_MAX_THREADS) pick.
 * A higher-priority thread that becomes ready preempts at the next tick or
 * yield; equal priorities rotate every `sched.quantum` ticks, least recently
 * served first. Everything here is tunable/observable at run time.
 */
enum state { UNUSED, READY, RUNNING, SLEEPING, DEAD };
static const char *const state_name[] = { "-", "READY", "RUN", "SLEEP", "DEAD" };

struct thread {
    const char *name;
    enum state state;
    u8 prio;
    u32 quantum;
    u64 wake, stamp, runticks, ready_tsc, lat_max;
    u64 stack; u32 stack_pages;
    u64 cr3, ktop;                 /* address space and kernel-stack top (TSS.rsp0) */
    void *proc;
    void *wait_key;
    struct uctx uc;
    struct frame *saved;
    u8 fx[512] __attribute__((aligned(16)));          /* FXSAVE area (user threads only) */
};

static struct thread threads[SCHED_MAX_THREADS];
static int current;
static u64 seq, total_run, cur_cr3, cur_fsbase, cur_gsbase;
static u8 fx_init[512] __attribute__((aligned(16)));
#define FXSAVE(p)  __asm__ volatile("fxsave (%0)" :: "r"(p) : "memory")
#define FXRSTOR(p) __asm__ volatile("fxrstor (%0)" :: "r"(p) : "memory")
static u64 rt_lat_max, rt_lat_sum, rt_lat_n;      /* TSC cycles, real-time class only */
static struct tunable *t_quantum, *t_stack, *t_rtprio, *t_latstats;

void sched_register_tunables(void)
{
    module_register("sched", "1.2", "fixed-priority preemptive scheduler, per-thread CPU accounting");
    t_quantum = tune_int("sched", "quantum", "round-robin slice for equal priorities (ticks)", TF_LIVE, 10, 1, 10000, NULL);
    t_stack   = tune_int("sched", "stack_pages", "kernel stack size of new threads (4 KiB pages)", TF_LIVE, 8, 2, 256, NULL);
    t_rtprio  = tune_int("sched", "rt_prio_max", "threads with priority <= this form the real-time class (latency is tracked for them)", TF_LIVE, 3, 0, 6, NULL);
    t_latstats = tune_bool("sched", "latency_stats", "measure ready->running latency (2 TSC reads per switch)", TF_LIVE, 1, NULL);
}

static void thread_exit(void)
{
    threads[current].state = DEAD;
    for (;;) thread_yield();
}

void sched_init(void)
{
    threads[0] = (struct thread){ .name = "idle", .state = RUNNING, .prio = SCHED_PRIO_IDLE, .cr3 = vm_kernel_pml4() };
    cur_cr3 = vm_kernel_pml4();
    current = 0;
    __asm__ volatile("fninit");
    FXSAVE(fx_init);                                       /* clean FPU/SSE state handed to every new user thread */
}

static int create(const char *name, u8 prio, thread_fn fn, void *arg, void *proc, u64 cr3, u64 urip, u64 ursp, const struct frame *init, const struct uctx *uc)
{
    u64 fl = irq_save();
    int slot = -1;
    for (int i = 1; i < SCHED_MAX_THREADS; i++)
        if (threads[i].state == UNUSED || threads[i].state == DEAD) { slot = i; break; }
    if (slot < 0) { irq_restore(fl); return -1; }
    struct thread *t = &threads[slot];
    if (t->stack) { pmm_free(t->stack, t->stack_pages); t->stack = 0; }      /* DEAD thread: stack no longer in use */
    u32 pages = (u32)t_stack->val;
    u64 stack = pmm_alloc(pages);
    if (!stack) { t->state = UNUSED; irq_restore(fl); return -1; }
    u64 top = (u64)P2V(stack) + (u64)pages * 4096;
    struct frame *f;
    if (proc) {                                             /* first run = iretq into ring 3 */
        f = (struct frame *)(top - sizeof *f);
        memset(f, 0, sizeof *f);
        f->rip = urip; f->cs = SEL_UCODE; f->ss = SEL_UDATA; f->rflags = 0x202; f->rsp = ursp;
        if (init) *f = *init;                                   /* complete register state is in place before the thread can run */
    } else {
        *(u64 *)(top - 8) = (u64)thread_exit;               /* return address of fn */
        f = (struct frame *)(top - 8 - sizeof *f);
        memset(f, 0, sizeof *f);
        f->rip = (u64)fn; f->rdi = (u64)arg;
        f->cs = SEL_KCODE; f->ss = SEL_KDATA; f->rflags = 0x202;
        f->rsp = top - 8;
    }
    *t = (struct thread){ .name = name, .state = READY, .prio = prio, .stamp = ++seq, .ready_tsc = rdtsc(),
                          .stack = stack, .stack_pages = pages, .cr3 = cr3, .ktop = top, .proc = proc, .saved = f };
    if (proc) { if (init) FXSAVE(t->fx); else memcpy(t->fx, fx_init, sizeof t->fx); }       /* a forked child inherits the parent's FPU/SSE registers */
    if (uc) t->uc = *uc;                                       /* TLS base and thread id are in place before it can run */
    irq_restore(fl);
    return slot;
}

int thread_create(const char *name, u8 prio, thread_fn fn, void *arg)
{
    return create(name, prio, fn, arg, NULL, vm_kernel_pml4(), 0, 0, NULL, NULL);
}

int thread_create_user(const char *name, u8 prio, void *proc, u64 cr3, u64 rip, u64 usp, const struct uctx *uc)
{
    return create(name, prio, NULL, NULL, proc, cr3, rip, usp, NULL, uc);
}

void *sched_current_proc(void) { return threads[current].proc; }

/* mark dead without switching (fault / exit paths return a frame to resume instead) */
void thread_mark_dead(int id)
{
    u64 fl = irq_save();
    if (id > 0 && id < SCHED_MAX_THREADS && threads[id].state != UNUSED) threads[id].state = DEAD;
    irq_restore(fl);
}

int thread_is_dead(int id) { return id <= 0 || id >= SCHED_MAX_THREADS || threads[id].state == DEAD || threads[id].state == UNUSED; }

int thread_self(void) { return current; }

int thread_set_prio(int id, int prio)
{
    if (id < 0 || id >= SCHED_MAX_THREADS || prio < 0 || prio > SCHED_PRIO_IDLE) return -1;
    u64 fl = irq_save();
    int ok = threads[id].state != UNUSED && threads[id].state != DEAD;
    if (ok) threads[id].prio = (u8)prio;
    irq_restore(fl);
    if (ok) thread_yield();         /* re-evaluate immediately (priority may have dropped or risen) */
    return ok ? 0 : -1;
}

int thread_kill(int id)
{
    if (id <= 0 || id >= SCHED_MAX_THREADS) return -1;
    u64 fl = irq_save();
    int ok = threads[id].state != UNUSED && threads[id].state != DEAD;
    if (ok) threads[id].state = DEAD;
    irq_restore(fl);
    if (ok && id == current) for (;;) thread_yield();
    return ok ? 0 : -1;
}

int thread_info(int id, struct thread_info *o)
{
    if (id < 0 || id >= SCHED_MAX_THREADS || threads[id].state == UNUSED) return -1;
    struct thread *t = &threads[id];
    *o = (struct thread_info){ id, t->name, state_name[t->state], t->prio, t->runticks, (int)(t->stack_pages * 4), cycles_to_ns(t->lat_max) };
    return 0;
}

u64 sched_runticks_total(void) { return total_run; }

static struct frame *switch_to_best(struct frame *cur, int yielded)
{
    struct thread *t = &threads[current];
    t->saved = cur;
    if (!yielded) { t->runticks++; total_run++; }

    u64 now = t_latstats->val ? rdtsc() : 0;
    int best = 255;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        struct thread *x = &threads[i];
        if (x->state == SLEEPING && x->wake <= ticks) { x->state = READY; x->ready_tsc = now; x->wait_key = NULL; }
        if ((x->state == READY || x->state == RUNNING) && x->prio < best) best = x->prio;
    }

    if (!yielded && t->state == RUNNING) {
        if (t->quantum) t->quantum--;
        if (t->quantum == 0) t->stamp = ++seq;      /* slice used up: go to the back */
        if (t->prio == best && t->quantum > 0) return cur;
    }
    if (yielded && t->state != DEAD) { t->stamp = ++seq; t->quantum = 0; }

    /* Round-robin within the best level: least recently served wins. */
    int next = -1;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        struct thread *x = &threads[i];
        if ((x->state == READY || x->state == RUNNING) && x->prio == best &&
            (next < 0 || x->stamp < threads[next].stamp))
            next = i;
    }
    if (next < 0) panic("scheduler: no runnable thread");
    if (t->state == RUNNING) { t->state = READY; t->ready_tsc = now; }
    if (now && next != current && threads[next].ready_tsc) {
        u64 lat = now - threads[next].ready_tsc;
        if (lat > threads[next].lat_max) threads[next].lat_max = lat;
        if (threads[next].prio <= t_rtprio->val) { rt_lat_sum += lat; rt_lat_n++; if (lat > rt_lat_max) rt_lat_max = lat; }
    }
    threads[next].state = RUNNING;
    if (threads[next].quantum == 0) {
        threads[next].quantum = (u32)t_quantum->val;
        threads[next].stamp = ++seq;
    }
    if (next != current) {
        if (t->proc) FXSAVE(t->fx);
        if (threads[next].proc) {
            FXRSTOR(threads[next].fx);
            if (threads[next].uc.gsbase != cur_gsbase) { cur_gsbase = threads[next].uc.gsbase; __asm__ volatile("wrmsr" :: "c"(0xC0000101), "a"((u32)cur_gsbase), "d"((u32)(cur_gsbase >> 32))); }
            if (threads[next].uc.fsbase != cur_fsbase) { cur_fsbase = threads[next].uc.fsbase; __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((u32)cur_fsbase), "d"((u32)(cur_fsbase >> 32))); }
        }
    }
    current = next;
    if (threads[next].cr3 != cur_cr3) { cur_cr3 = threads[next].cr3; vm_switch(cur_cr3); }
    if (threads[next].ktop) tss_set_rsp0(threads[next].ktop);
    return threads[next].saved;
}

struct frame *sched_on_tick(struct frame *f)  { return switch_to_best(f, 0); }
struct frame *sched_on_yield(struct frame *f) { return switch_to_best(f, 1); }

void thread_yield(void) { __asm__ volatile("int %0" :: "i"(NEOS_YIELD_VECTOR) : "memory"); }

void thread_sleep_until(u64 tick)
{
    u64 fl = irq_save();
    if (ticks < tick) { threads[current].wake = tick; threads[current].state = SLEEPING; }
    irq_restore(fl);
    if (threads[current].state == SLEEPING) thread_yield();
}

void thread_sleep_ms(u64 ms) { thread_sleep_until(ticks + ms_to_ticks(ms)); }

void sched_rescale(u32 old_hz, u32 new_hz)
{
    u64 fl = irq_save();
    ticks = ticks * new_hz / old_hz;
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (threads[i].state == SLEEPING) threads[i].wake = threads[i].wake * new_hz / old_hz;
    irq_restore(fl);
}

int thread_protected(int id)
{
    static const char *const core[] = { "idle", "console", "net", "varbus", "init", "agentd", "ui", "shell" };
    if (id < 0 || id >= SCHED_MAX_THREADS || threads[id].state == UNUSED) return 1;
    for (unsigned i = 0; i < sizeof core / sizeof *core; i++) if (!strcmp(threads[id].name, core[i])) return 1;
    return 0;
}

void sched_latency(u64 *max_ns, u64 *avg_ns, u64 *n)
{
    u64 fl = irq_save();
    *n = rt_lat_n;
    *max_ns = cycles_to_ns(rt_lat_max);
    *avg_ns = rt_lat_n ? cycles_to_ns(rt_lat_sum / rt_lat_n) : 0;
    irq_restore(fl);
}

void sched_latency_reset(void)
{
    u64 fl = irq_save();
    rt_lat_max = rt_lat_sum = rt_lat_n = 0;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) threads[i].lat_max = 0;
    irq_restore(fl);
}

struct uctx *uctx_current(void) { return &threads[current].uc; }
struct uctx *uctx_of(int id) { return id > 0 && id < SCHED_MAX_THREADS ? &threads[id].uc : NULL; }
int thread_current_id(void) { return current; }

void sleep_on_locked(void *key, u64 timeout_tick)
{
    threads[current].wait_key = key;
    threads[current].wake = timeout_tick;
    threads[current].state = SLEEPING;
    thread_yield();
}

int wake_on(void *key, int max)
{
    int n = 0;
    u64 fl = irq_save();
    u64 now = rdtsc();
    for (int i = 0; i < SCHED_MAX_THREADS && n < max; i++)
        if (threads[i].state == SLEEPING && threads[i].wait_key == key) { threads[i].state = READY; threads[i].wait_key = NULL; threads[i].ready_tsc = now; n++; }
    irq_restore(fl);
    return n;
}

int thread_create_user_frame(const char *name, u8 prio, void *proc, u64 cr3, const struct frame *init, const struct uctx *uc)
{
    return create(name, prio, NULL, NULL, proc, cr3, init->rip, init->rsp, init, uc);
}

void sched_set_fsbase(u64 v)
{
    threads[current].uc.fsbase = v;
    cur_fsbase = v;
    __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((u32)v), "d"((u32)(v >> 32)));
}

void sched_switch_cr3(u64 cr3)                        /* the running thread gets a new address space (execve) */
{
    u64 fl = irq_save();
    threads[current].cr3 = cr3; cur_cr3 = cr3; vm_switch(cr3);
    irq_restore(fl);
}

void sched_kill_threads_of(void *proc)
{
    u64 fl = irq_save();
    for (int i = 1; i < SCHED_MAX_THREADS; i++) if (threads[i].proc == proc && threads[i].state != UNUSED) threads[i].state = DEAD;
    irq_restore(fl);
}

int sched_threads_alive(void *proc)
{
    int n = 0;
    for (int i = 1; i < SCHED_MAX_THREADS; i++) if (threads[i].proc == proc && threads[i].state != UNUSED && threads[i].state != DEAD) n++;
    return n;
}

int thread_prio_of(int id) { return id >= 0 && id < SCHED_MAX_THREADS ? threads[id].prio : 4; }

void sched_kill_threads_of_except(void *proc, int keep)
{
    u64 fl = irq_save();
    for (int i = 1; i < SCHED_MAX_THREADS; i++) if (i != keep && threads[i].proc == proc && threads[i].state != UNUSED) threads[i].state = DEAD;
    irq_restore(fl);
}

int requeue_on(void *from, void *to, int max)
{
    int n = 0;
    u64 fl = irq_save();
    for (int i = 0; i < SCHED_MAX_THREADS && n < max; i++)
        if (threads[i].state == SLEEPING && threads[i].wait_key == from) { threads[i].wait_key = to; n++; }
    irq_restore(fl);
    return n;
}

void sched_dump(void)
{
    kprintf("\n\x1b[1;93m-- threads --\x1b[0m\n");
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state == UNUSED) continue;
        kprintf(" %2d %-9s prio=%d %-6s", i, t->name, t->prio, state_name[t->state]);
        if (t->state == SLEEPING) kprintf(" wake=%s key=%p", t->wake == ~0ull ? "never" : "timer", t->wait_key);
        if (t->proc) kprintf(" last_syscall=%d utid=%d", t->uc.last_sys, t->uc.utid);
        kprintf("\n");
    }
}

void sched_set_gsbase(u64 v)
{
    threads[current].uc.gsbase = v;
    cur_gsbase = v;
    __asm__ volatile("wrmsr" :: "c"(0xC0000101), "a"((u32)v), "d"((u32)(v >> 32)));
}
