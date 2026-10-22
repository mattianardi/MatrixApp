#include "kernel.h"

/* Real-time self-test: priority preemption, bounded wake latency under CPU
 * saturation, starvation of the lowest level, fairness inside one level. */
static volatile u64 hog_count, a_count, b_count;
static volatile int rt_done;
static volatile u64 rt_max_latency;

#define RT_PERIOD_MS 10
#define RT_ITERS     30

static void hog(void *arg)    { (void)arg; for (;;) hog_count++; }
static void work_a(void *arg) { (void)arg; for (;;) a_count++; }
static void work_b(void *arg) { (void)arg; for (;;) b_count++; }

static void rt_task(void *arg)
{
    (void)arg;
    u64 per = ms_to_ticks(RT_PERIOD_MS), t0 = ticks;
    for (int i = 1; i <= RT_ITERS; i++) {
        u64 due = t0 + (u64)i * per;
        thread_sleep_until(due);
        u64 lat = ticks - due;
        if (lat > rt_max_latency) rt_max_latency = lat;
    }
    rt_done = 1;
}

int selftest_run(void)
{
    int self = thread_self(), old_prio = 4;
    struct thread_info ti;
    if (!thread_info(self, &ti)) old_prio = ti.prio;
    hog_count = a_count = b_count = 0; rt_done = 0; rt_max_latency = 0;

    thread_set_prio(self, 2);               /* above the workers, below the rt task */
    int ids[4];
    ids[0] = thread_create("t_rt",   1, rt_task, 0);
    ids[1] = thread_create("t_a",    3, work_a,  0);
    ids[2] = thread_create("t_b",    3, work_b,  0);
    ids[3] = thread_create("t_hog",  5, hog,     0);
    thread_sleep_ms(RT_ITERS * RT_PERIOD_MS + 100);
    for (int i = 0; i < 4; i++) if (ids[i] > 0) thread_kill(ids[i]);
    thread_set_prio(self, old_prio);

    u64 lo = a_count < b_count ? a_count : b_count, hi = a_count < b_count ? b_count : a_count;
    kprintf("[test] hog=%lu a=%lu b=%lu\n", hog_count, a_count, b_count);
    kprintf("[test] rt periodic task: %d periods, max wake latency %lu tick(s)\n", RT_ITERS, rt_max_latency);
    int ok = rt_done && rt_max_latency == 0 && hog_count == 0 && lo && lo * 10 >= hi * 9;
    kprintf(ok ? "SELFTEST PASS\n" : "SELFTEST FAIL\n");
    return ok ? 0 : 1;
}
