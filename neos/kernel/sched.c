#include "kernel.h"
#include "neos_api.h"

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
    u64 wake, stamp, runticks;
    u64 stack; u32 stack_pages;
    struct frame *saved;
};

static struct thread threads[SCHED_MAX_THREADS];
static int current;
static u64 seq, total_run;
static struct tunable *t_quantum, *t_stack;

void sched_register_tunables(void)
{
    module_register("sched", "1.2", "fixed-priority preemptive scheduler, per-thread CPU accounting");
    t_quantum = tune_int("sched", "quantum", "round-robin slice for equal priorities (ticks)", TF_LIVE, 10, 1, 10000, NULL);
    t_stack   = tune_int("sched", "stack_pages", "kernel stack size of new threads (4 KiB pages)", TF_LIVE, 8, 2, 256, NULL);
}

static void thread_exit(void)
{
    threads[current].state = DEAD;
    for (;;) thread_yield();
}

void sched_init(void)
{
    threads[0] = (struct thread){ .name = "idle", .state = RUNNING, .prio = SCHED_PRIO_IDLE };
    current = 0;
}

int thread_create(const char *name, u8 prio, thread_fn fn, void *arg)
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
    u64 top = stack + (u64)pages * 4096;
    *(u64 *)(top - 8) = (u64)thread_exit;                   /* return address of fn */
    struct frame *f = (struct frame *)(top - 8 - sizeof *f);
    memset(f, 0, sizeof *f);
    f->rip = (u64)fn; f->rdi = (u64)arg;
    f->cs = 0x08; f->ss = 0x10; f->rflags = 0x202;
    f->rsp = top - 8;
    *t = (struct thread){ .name = name, .state = READY, .prio = prio, .stamp = ++seq,
                          .stack = stack, .stack_pages = pages, .saved = f };
    irq_restore(fl);
    return slot;
}

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
    *o = (struct thread_info){ id, t->name, state_name[t->state], t->prio, t->runticks, (int)(t->stack_pages * 4) };
    return 0;
}

u64 sched_runticks_total(void) { return total_run; }

static struct frame *switch_to_best(struct frame *cur, int yielded)
{
    struct thread *t = &threads[current];
    t->saved = cur;
    if (!yielded) { t->runticks++; total_run++; }

    int best = 255;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        struct thread *x = &threads[i];
        if (x->state == SLEEPING && x->wake <= ticks) x->state = READY;
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
    if (t->state == RUNNING) t->state = READY;
    threads[next].state = RUNNING;
    if (threads[next].quantum == 0) {
        threads[next].quantum = (u32)t_quantum->val;
        threads[next].stamp = ++seq;
    }
    current = next;
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
