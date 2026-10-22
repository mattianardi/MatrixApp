#include "kernel.h"
#include "shell.h"
#include "varbus.h"
#include "net.h"
#include "agent.h"
#include "proc.h"
#include "aih.h"
#include "fs.h"
#include "vfs.h"

static struct tunable *t_test;
static u32 boot_mods[4][2];          /* copied before any allocation: QEMU puts the multiboot structures in memory we would reuse */
static int boot_nmods;

static void fmt_cpu(char *b, size_t n)  { ksnprintf(b, n, "%s%s", cpu_brand(), cpu_invariant_tsc() ? "  (invariant TSC)" : ""); }
static void fmt_mem(char *b, size_t n)
{
    size_t u, p, t; heap_stats(&u, &p, &t);
    ksnprintf(b, n, "%lu MiB usable, %lu MiB free, kernel heap %lu MiB",
              (u64)pmm_total_pages() / 256, (u64)pmm_free_pages() / 256, (u64)t >> 20);
}
static void fmt_sched(char *b, size_t n) { ksnprintf(b, n, "fixed-priority preemptive, %u Hz tick", timer_hz()); }
static void fmt_net(char *b, size_t n)   { if (!net_nic_present()) { strlcpy(b, "no supported NIC found", n); return; } struct tunable *m = tune_find("net.mac"); ksnprintf(b, n, "e1000 %s (DHCP at boot: %s)", m->sval, net_wants_dhcp() ? "on" : "off"); }
static void fmt_sec(char *b, size_t n)   { ksnprintf(b, n, "ring 3 processes, NX, SMEP %s, SMAP %s, kernel in the higher half", vm_smep() ? "on" : "off", vm_smap() ? "on" : "off"); }
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

static void load_initrd(void)
{
    for (int i = 0; i < boot_nmods; i++) {
        const char *e = "";
        int n = pkg_extract_root(P2V(boot_mods[i][0]), boot_mods[i][1] - boot_mods[i][0], &e);
        if (n < 0) kprintf("[initrd] module %d rejected: %s\n", i, e);
        else kprintf("[initrd] %d files unpacked (%u KiB)\n", n, (boot_mods[i][1] - boot_mods[i][0]) >> 10);
    }
}

/* Linux userland: every BusyBox applet becomes a name in /bin, so `grep`, `awk`, `sed`... work from the NEOS shell */
static void install_applets(void)
{
    static const char *const applets[] = { "ash", "sh", "awk", "basename", "cat", "chmod", "cp", "cut", "date", "df", "diff", "dirname", "du", "env", "expr", "false",
        "find", "grep", "egrep", "head", "hexdump", "hostname", "id", "kill", "ln", "md5sum", "sha256sum", "mkdir", "mv", "od", "printf", "readlink", "rm", "rmdir", "sed", "seq",
        "sleep", "sort", "stat", "tail", "tar", "tee", "test", "[", "touch", "tr", "true", "uname", "uniq", "wc", "which", "xargs", "yes", "less", "more", "vi", "gzip", "gunzip",
        "echo", "pwd", "base64", "cmp", "comm", "cpio", "dd", "fold", "free", "ls", "nl", "paste", "realpath", "rev", "shuf", "split", "strings", "sync", "tac", "tree", "unzip", "uptime", "watch", "xxd" };
    struct inode *bb = vfs_lookup("/bin/busybox");
    if (!bb) return;
    int n = 0;
    for (unsigned i = 0; i < sizeof applets / sizeof *applets; i++) {
        char p[64]; ksnprintf(p, sizeof p, "/bin/%s", applets[i]);
        if (!vfs_lookup(p) && !vfs_symlink("busybox", p)) n++;
    }
    kprintf("[linux] %d BusyBox applets linked in /bin\n", n);
}

static void init_thread(void *arg)
{
    (void)arg;
    providers_init();
    load_initrd();
    neofs_load();
    install_applets();
    fs_write("/etc/hosts", "127.0.0.1 localhost\n10.0.2.2 host\n", 34);
    fs_write("/etc/nsswitch.conf", "hosts: files dns\n", 18);
    fs_write("/etc/resolv.conf", "nameserver 10.0.2.3\n", 20);
    if (t_test->val) { int rc = selftest_run(); qemu_exit(rc ? 0x11 : 0x10); }
    agentd_start();
    neofs_start();
    shell_start();
    ui_start();
    gui_start();
    if (net_nic_present() && net_wants_dhcp()) {
        net_start();
        if (net_dhcp() == 0) { char b[24]; ip_format(net_ip(), b, sizeof b); kprintf("\n[net] DHCP: %s\n", b); }
        else kprintf("\n[net] no DHCP answer; use `net set` or `net dhcp`\n");
    } else if (net_nic_present()) net_start();
}

const u32 *boot_mbi;                    /* the loader's multiboot info (framebuffer, modules) */

void kmain(u32 magic, u64 mbi_phys)
{
    const void *mbi = P2V(mbi_phys);
    boot_mbi = mbi;
    console_init();                       /* synchronous until console_async_start() */
    if (magic != 0x2BADB002) panic("bad multiboot magic %x", magic);
    vm_init();                            /* drop the boot identity map: the lower half is for user processes */
    gdt_init();
    syscall_init();

    const u32 *mb = mbi;
    if (mb[0] & (1 << 3)) {
        const u32 *mods = P2V(mb[6]);
        for (u32 i = 0; i < mb[5] && i < 4; i++) { boot_mods[boot_nmods][0] = mods[i * 4]; boot_mods[boot_nmods][1] = mods[i * 4 + 1]; boot_nmods++; }
    }
    if (mb[0] & (1 << 2)) tune_bootargs((const char *)P2V(mb[4]));      /* kernel command line */
    module_register("kernel", NEOS_VERSION, "boot, interrupts, API");
    t_test = tune_bool("boot", "test", "run the RT self-test and exit QEMU (CI)", TF_BOOT, 0, NULL);

    vm_tunables();
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
    neofs_init();
    net_init();
    bus_init();
    agent_init();
    proc_init();
    aih_init();
    agent_add("aih", AK_NATIVE, NULL, NULL, 0, &(const char *){ "" });
    linux_init();
    ui_init();
    gui_init();
    console_async_start();
    bus_start();

    splash_info("Build",     fmt_build);
    splash_info("CPU",       fmt_cpu);
    splash_info("Memory",    fmt_mem);
    splash_info("Scheduler", fmt_sched);
    splash_info("Security",  fmt_sec);
    splash_info("Network",   fmt_net);
    splash_info("Modules",   fmt_mods);
    splash_show();

    thread_create("init", 3, init_thread, 0);

    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");     /* idle thread */
}
