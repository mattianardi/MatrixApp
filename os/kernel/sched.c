#include "kernel.h"

/*
 * Deterministic scheduler: fixed priorities, no dynamic allocation or priority
 * inheritance tricks in the dispatch path, bounded O(SCHED_MAX_THREADS) pick.
 * A higher-priority thread that becomes ready always preempts at the next tick
 * (or yield); equal priorities are rotated every SCHED_QUANTUM ticks.
 */
enum state { UNUSED, READY, RUNNING, SLEEPING, DEAD };

struct thread {
    const char *name;
    enum state state;
    u8 prio;
    u32 quantum;
    u64 wake;
    u64 stamp;              /* start of current slice; smallest = least recently served */
    struct frame *saved;
};

static struct thread threads[SCHED_MAX_THREADS];
static int current;
static u64 seq;

#define STACK_PAGES 4

static void thread_exit(void)
{
    threads[current].state = DEAD;
    for (;;) thread_yield();
}

void sched_init(void)
{
    /* thread 0 is the boot context; it becomes the idle thread */
    threads[0] = (struct thread){ .name = "idle", .state = RUNNING, .prio = SCHED_PRIO_IDLE };
    current = 0;
}

int thread_create(const char *name, u8 prio, thread_fn fn, void *arg)
{
    for (int i = 1; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state != UNUSED && threads[i].state != DEAD) continue;
        u64 stack = pmm_alloc(STACK_PAGES);
        if (!stack) return -1;
        u64 top = stack + STACK_PAGES * 4096;
        *(u64 *)(top - 8) = (u64)thread_exit;           /* return address of fn */
        struct frame *f = (struct frame *)(top - 8 - sizeof *f);
        memset(f, 0, sizeof *f);
        f->rip = (u64)fn; f->rdi = (u64)arg;
        f->cs = 0x08; f->ss = 0x10; f->rflags = 0x202;
        f->rsp = top - 8;
        threads[i] = (struct thread){ .name = name, .state = READY, .prio = prio, .stamp = ++seq, .saved = f };
        return i;
    }
    return -1;
}

static struct frame *switch_to_best(struct frame *cur, int yielded)
{
    struct thread *t = &threads[current];
    t->saved = cur;

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
        threads[next].quantum = SCHED_QUANTUM;
        threads[next].stamp = ++seq;
    }
    current = next;
    return threads[next].saved;
}

struct frame *sched_on_tick(struct frame *f)  { return switch_to_best(f, 0); }
struct frame *sched_on_yield(struct frame *f) { return switch_to_best(f, 1); }

void thread_yield(void) { __asm__ volatile("int $0x81" ::: "memory"); }

void thread_sleep_until(u64 tick)
{
    if (ticks >= tick) return;
    threads[current].wake = tick;
    threads[current].state = SLEEPING;
    thread_yield();
}
