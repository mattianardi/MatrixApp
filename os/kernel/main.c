#include "kernel.h"

/* ---- boot-time self-test: priority preemption + bounded wake latency ---- */
static volatile u64 hog_count, a_count, b_count;
static volatile int rt_done;
static volatile u64 rt_max_latency;

#define RT_PERIOD 10
#define RT_ITERS  30

static void hog(void *arg)  { (void)arg; for (;;) hog_count++; }
static void work_a(void *arg) { (void)arg; for (;;) a_count++; }
static void work_b(void *arg) { (void)arg; for (;;) b_count++; }

static void rt_task(void *arg)
{
    (void)arg;
    u64 t0 = ticks;
    for (int i = 1; i <= RT_ITERS; i++) {
        u64 due = t0 + (u64)i * RT_PERIOD;
        thread_sleep_until(due);
        u64 lat = ticks - due;
        if (lat > rt_max_latency) rt_max_latency = lat;
    }
    rt_done = 1;
}

static void control(void *arg)
{
    (void)arg;
    thread_sleep_until(ticks + 400);
    kprintf("[test] hog=%lu a=%lu b=%lu\n", hog_count, a_count, b_count);
    kprintf("[test] rt periodic task: %d periods, max wake latency %lu tick(s)\n",
            RT_ITERS, rt_max_latency);
    /* hog (prio 5) must be starved by the prio-3 workers, which must share the CPU fairly */
    u64 lo = a_count < b_count ? a_count : b_count, hi = a_count < b_count ? b_count : a_count;
    int ok = rt_done && rt_max_latency == 0 && hog_count == 0 && lo && lo * 10 >= hi * 9;
    kprintf(ok ? "SELFTEST PASS\n" : "SELFTEST FAIL\n");
    qemu_exit(ok ? 0x10 : 0x11);
}

void kmain(u32 magic, const void *mbi)
{
    console_init();
    kprintf("MatrixOS 0.1 - x86_64 deterministic kernel\n");
    if (magic != 0x2BADB002) panic("bad multiboot magic %x", magic);

    pmm_init(mbi);
    kprintf("[mem] %lu KiB free\n", (u64)pmm_free_pages() * 4);

    idt_init();
    pic_init();
    pit_init(TICK_HZ);
    sched_init();

    thread_create("control", 2, control, 0);
    thread_create("rt",      1, rt_task, 0);
    thread_create("work_a",  3, work_a,  0);
    thread_create("work_b",  3, work_b,  0);
    thread_create("hog",     5, hog,     0);

    kprintf("[sched] %d Hz tick, fixed-priority preemptive\n", TICK_HZ);
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");   /* idle thread */
}
