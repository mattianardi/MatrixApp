#include "kernel.h"

volatile u64 ticks;
static u32 hz = 1000;
static u64 tick_ns = 1000000;
static u64 now_ns, last_tick_tsc, tsc_per_sec;
static u64 cal_tsc0, cal_ns0;
static u64 j_min = ~0ull, j_max, j_sum, j_n;      /* inter-tick period, TSC cycles */
static struct tunable *t_hz;

static int hz_cb(struct tunable *t, i64 v, const char *s)
{
    (void)t; (void)s;
    u32 old = hz;
    timer_set_hz((u32)v);
    sched_rescale(old, hz);
    return 0;
}

void timer_init(void)
{
    module_register("timer", "1.1", "PIT tick, monotonic clock, TSC calibration, tick-jitter statistics");
    t_hz = tune_int("timer", "hz", "scheduler tick frequency (Hz); higher = lower latency, more overhead",
                    TF_LIVE, 1000, 100, 20000, hz_cb);
    timer_set_hz((u32)t_hz->val);
}

void timer_set_hz(u32 want)
{
    u32 div = 1193182 / want;
    if (div < 1) div = 1;
    if (div > 65535) div = 65535;
    u64 fl = irq_save();
    hz = (u32)(1193182 / div);
    tick_ns = (u64)div * 1000000000ull / 1193182ull;
    outb(0x43, 0x34); outb(0x40, div & 0xFF); outb(0x40, div >> 8);
    j_min = ~0ull; j_max = j_sum = j_n = 0; last_tick_tsc = 0;
    tsc_per_sec = 0; cal_tsc0 = 0;
    irq_restore(fl);
}

u32 timer_hz(void) { return hz; }
u64 ms_to_ticks(u64 ms) { u64 t = ms * hz / 1000; return t ? t : 1; }

void timer_tick(void)
{
    u64 t = rdtsc();
    ticks++;
    now_ns += tick_ns;
    if (last_tick_tsc) {
        u64 d = t - last_tick_tsc;
        if (d < j_min) j_min = d;
        if (d > j_max) j_max = d;
        j_sum += d; j_n++;
    }
    last_tick_tsc = t;
    if (!cal_tsc0) { cal_tsc0 = t; cal_ns0 = now_ns; }
    else if (!tsc_per_sec && now_ns - cal_ns0 >= 200000000ull)       /* 200 ms window */
        tsc_per_sec = (t - cal_tsc0) * 1000000000ull / (now_ns - cal_ns0);
}

u64 tsc_hz(void) { return tsc_per_sec; }

u64 time_ns(void)
{
    u64 fl = irq_save();
    u64 n = now_ns;
    if (tsc_per_sec && last_tick_tsc) n += (rdtsc() - last_tick_tsc) * 1000000000ull / tsc_per_sec;
    irq_restore(fl);
    return n;
}

static u64 cyc_to_ns(u64 c) { return tsc_per_sec ? c * 1000000000ull / tsc_per_sec : c; }

void timer_jitter(u64 *mn, u64 *mx, u64 *avg, u64 *n)
{
    u64 fl = irq_save();
    *n = j_n;
    *mn = j_n ? cyc_to_ns(j_min) : 0;
    *mx = cyc_to_ns(j_max);
    *avg = j_n ? cyc_to_ns(j_sum / j_n) : 0;
    irq_restore(fl);
}

void timer_jitter_reset(void)
{
    u64 fl = irq_save();
    j_min = ~0ull; j_max = j_sum = j_n = 0;
    irq_restore(fl);
}
