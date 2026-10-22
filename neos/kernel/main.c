#include "kernel.h"
#include "shell.h"
#include "varbus.h"
#include "net.h"
#include "fs.h"

static struct tunable *t_test;

static void fmt_cpu(char *b, size_t n)  { ksnprintf(b, n, "%s%s", cpu_brand(), cpu_invariant_tsc() ? "  (invariant TSC)" : ""); }
static void fmt_mem(char *b, size_t n)
{
    size_t u, p, t; heap_stats(&u, &p, &t);
    ksnprintf(b, n, "%lu MiB usable, %lu MiB free, kernel heap %lu MiB",
              (u64)pmm_total_pages() / 256, (u64)pmm_free_pages() / 256, (u64)t >> 20);
}
static void fmt_sched(char *b, size_t n) { ksnprintf(b, n, "fixed-priority preemptive, %u Hz tick", timer_hz()); }
static void fmt_net(char *b, size_t n)   { if (!net_nic_present()) { strlcpy(b, "no supported NIC found", n); return; } struct tunable *m = tune_find("net.mac"); ksnprintf(b, n, "e1000 %s (DHCP at boot: %s)", m->sval, net_wants_dhcp() ? "on" : "off"); }
static void fmt_mods(char *b, size_t n)  { ksnprintf(b, n, "%d modules, %d tunable settings", module_count(), tune_count()); }
static void fmt_build(char *b, size_t n) { ksnprintf(b, n, "x86_64, built %s", __DATE__); }

static void p_version(char *b, size_t n)  { strlcpy(b, NEOS_VERSION, n); }
static void p_uptime(char *b, size_t n)   { ksnprintf(b, n, "%lu", time_ns() / 1000000); }
static void p_timens(char *b, size_t n)   { ksnprintf(b, n, "%lu", time_ns()); }
static void p_memfree(char *b, size_t n)  { ksnprintf(b, n, "%lu", (u64)pmm_free_pages() * 4); }
static void p_jitter(char *b, size_t n)   { u64 a, m, v, c; timer_jitter(&a, &m, &v, &c); ksnprintf(b, n, "%lu", m - a); }

static void providers_init(void)
{   /* runs in the init thread: sync bus writes sleep, the idle thread must not */
    bus_provider("version", p_version);
    bus_provider("uptime_ms", p_uptime);
    bus_provider("time_ns", p_timens);
    bus_provider("mem.free_kib", p_memfree);
    bus_provider("rt.jitter_ns", p_jitter);
    /* defaults both worlds expect */
    const char *e;
    bus_set("HOME", "/mnt/c/Users/neos", NEOS_VIEW_LINUX, 1, &e);
    bus_set("PATH", "/usr/bin:/bin:/mnt/c/Windows/System32", NEOS_VIEW_LINUX, 1, &e);
    bus_set("USER", "neos", NEOS_VIEW_LINUX, 1, &e);
    bus_set("HOSTNAME", "neos", NEOS_VIEW_LINUX, 1, &e);
    bus_set("PWD", "/", NEOS_VIEW_LINUX, 1, &e);
}

static void init_thread(void *arg)
{
    (void)arg;
    providers_init();
    if (t_test->val) { int rc = selftest_run(); qemu_exit(rc ? 0x11 : 0x10); }
    shell_start();
    ui_start();
    if (net_nic_present() && net_wants_dhcp()) {
        net_start();
        if (net_dhcp() == 0) { char b[24]; ip_format(net_ip(), b, sizeof b); kprintf("\n[net] DHCP: %s\n", b); }
        else kprintf("\n[net] no DHCP answer; use `net set` or `net dhcp`\n");
    } else if (net_nic_present()) net_start();
}

void kmain(u32 magic, const void *mbi)
{
    console_init();                       /* synchronous until console_async_start() */
    if (magic != 0x2BADB002) panic("bad multiboot magic %x", magic);

    const u32 *mb = mbi;
    if (mb[0] & (1 << 2)) tune_bootargs((const char *)(u64)mb[4]);      /* kernel command line */
    module_register("kernel", NEOS_VERSION, "boot, interrupts, API");
    t_test = tune_bool("boot", "test", "run the RT self-test and exit QEMU (CI)", TF_BOOT, 0, NULL);

    pmm_init(mbi);
    heap_init();
    idt_init();
    pic_init();
    timer_init();
    sched_register_tunables();
    sched_init();
    module_register("api", "1", "syscall int 0x80 + struct neos_api");
    input_init();
    fs_init();
    pkg_init();
    net_init();
    bus_init();
    ui_init();
    console_async_start();
    bus_start();

    splash_info("Build",     fmt_build);
    splash_info("CPU",       fmt_cpu);
    splash_info("Memory",    fmt_mem);
    splash_info("Scheduler", fmt_sched);
    splash_info("Network",   fmt_net);
    splash_info("Modules",   fmt_mods);
    splash_show();

    thread_create("init", 3, init_thread, 0);

    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");     /* idle thread */
}
