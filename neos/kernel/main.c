#include "kernel.h"
#include "usb.h"
#include "serial.h"
void evdev_init(void);
void fbdev_init(void);
#include "shell.h"
#include "varbus.h"
#include "net.h"
#include "agent.h"
#include "proc.h"
#include "aih.h"
#include "fs.h"
#include "vfs.h"

static struct tunable *t_test;
extern const u8 _binary_embedded_initrd_start[] __attribute__((weak)), _binary_embedded_initrd_end[] __attribute__((weak));   /* web build: initrd linked into the image */
static int initrd_embedded;
static u32 boot_mods[8][2];          /* copied before any allocation: QEMU puts the multiboot structures in memory we would reuse */
static int boot_nmods;

static void fmt_cpu(char *b, size_t n)  { ksnprintf(b, n, "%s%s", cpu_brand(), cpu_invariant_tsc() ? "  (invariant TSC)" : ""); }
static void fmt_mem(char *b, size_t n)
{
    size_t u, p, t; heap_stats(&u, &p, &t);
    ksnprintf(b, n, "%lu MiB usable, %lu MiB free, kernel heap %lu MiB",
              (u64)pmm_total_pages() / 256, (u64)pmm_free_pages() / 256, (u64)t >> 20);
}
static void fmt_sched(char *b, size_t n) { ksnprintf(b, n, "fixed-priority preemptive, %u Hz tick", timer_hz()); }
static void fmt_net(char *b, size_t n)   { if (!net_nic_present()) { strlcpy(b, "no supported NIC found", n); return; } struct tunable *m = tune_find("net.mac"); ksnprintf(b, n, "%s %s (DHCP at boot: %s)", nic_name(), m->sval, net_wants_dhcp() ? "on" : "off"); }
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
    bus_set("PATH", "/opt/bin:/usr/bin:/bin:/mnt/c/Windows/System32", NEOS_VIEW_LINUX, 1, &e);
    bus_set("USER", "neos", NEOS_VIEW_LINUX, 1, &e);
    bus_set("HOSTNAME", "neos", NEOS_VIEW_LINUX, 1, &e);
    bus_set("PWD", "/", NEOS_VIEW_LINUX, 1, &e);
}

static void load_initrd(void)
{
    for (int i = 0; i < boot_nmods; i++) {
        const char *e = "";
        u64 start = boot_mods[i][0], end = boot_mods[i][1];
        u32 kib = (u32)((end - start) >> 10);
        if (i == 0) {                                                  /* module 0 is the initrd: unpacked into the filesystem root */
            int n = pkg_extract_root(P2V(start), end - start, &e);
            if (n < 0) { kprintf("[initrd] module %d rejected: %s\n", i, e); continue; }
            kprintf("[initrd] %d files unpacked (%u KiB)\n", n, kib);
        } else if (pkg_is_root_module(P2V(start), end - start)) {       /* a package marked `extract=root` in its .pkginfo is unpacked into the filesystem like the initrd */
            int n = pkg_extract_root(P2V(start), end - start, &e);
            if (n < 0) { kprintf("[initrd] module %d rejected: %s\n", i, e); continue; }
            kprintf("[initrd] module %d: %d files unpacked (%u KiB)\n", i, n, kib);
        } else {                                                        /* further modules are packages for the offline cache */
            if (pkg_stage_cache(P2V(start), end - start, &e)) { kprintf("[cache] module %d rejected: %s\n", i, e); continue; }
            kprintf("[cache] package staged (%u KiB): `pkg install` works offline\n", kib);
        }
        if (initrd_embedded && i == 0) continue;                       /* part of the kernel image: nothing to give back */
        pmm_free(start & ~0xFFFull, ((end + 0xFFF) & ~0xFFFull) / 4096 - (start & ~0xFFFull) / 4096);     /* the copy in the filesystem is enough: give the boot memory back */
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
    vfs_mkdir("/dev/shm", 01777); vfs_mkdir("/run", 0755); vfs_mkdir("/var/tmp", 01777);
    vfs_mkdir("/usr/bin", 0755);                                           /* #!/usr/bin/env python3 and friends */
    if (!vfs_lookup("/usr/bin/env")) vfs_symlink("/bin/busybox", "/usr/bin/env");
    vfs_mkdir("/opt/bin", 0755); vfs_mkdir("/var/cache/pkg", 0755); vfs_mkdir("/var/pkg", 0755);
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
    usb_init();
    usb_start();
    serial_hw_init();
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

static void mark(int i, char c) { ((volatile u16 *)P2V(0xB8000))[24 * 80 + 54 + i] = 0x4F00 | (u8)c; }   /* boot breadcrumb on the last text row */

void kmain(u32 magic, u64 mbi_phys)
{
    const void *mbi = P2V(mbi_phys);
    boot_mbi = mbi;
    console_init();                       /* synchronous until console_async_start() */ mark(0,'E');
    if (magic != 0x2BADB002) panic("bad multiboot magic %x", magic);
    vm_init();                            /* drop the boot identity map: the lower half is for user processes */ mark(1,'F');
    gdt_init(); mark(2,'G');
    syscall_init(); mark(3,'H');

    const u32 *mb = mbi;
    if (mb[0] & (1 << 3)) {
        const u32 *mods = P2V(mb[6]);
        for (u32 i = 0; i < mb[5] && i < 8; i++) { boot_mods[boot_nmods][0] = mods[i * 4]; boot_mods[boot_nmods][1] = mods[i * 4 + 1]; boot_nmods++; }
    }
    if (!boot_nmods && &_binary_embedded_initrd_start[0] != &_binary_embedded_initrd_end[0]) {      /* loader without module support (browser emulators) */
        boot_mods[0][0] = (u32)V2P(_binary_embedded_initrd_start); boot_mods[0][1] = (u32)V2P(_binary_embedded_initrd_end); boot_nmods = 1; initrd_embedded = 1;
    }
    if (mb[0] & (1 << 2)) tune_bootargs((const char *)P2V(mb[4]));      /* kernel command line */
    module_register("kernel", NEOS_VERSION, "boot, interrupts, API");
    t_test = tune_bool("boot", "test", "run the RT self-test and exit QEMU (CI)", TF_BOOT, 0, NULL);

    vm_tunables();
    pmm_init(mbi); mark(4,'I');
    heap_init(); mark(5,'J');
    idt_init(); mark(6,'K');
    pic_init(); mark(7,'L');
    timer_init(); mark(8,'M');
    sched_register_tunables();
    sched_init(); mark(9,'N');
    module_register("api", "1", "syscall int 0x80 + struct neos_api");
    input_init(); mark(10,'O');
    fs_init(); mark(11,'P');
    pkg_init();
    neofs_init();
    net_init(); mark(12,'Q');
    bus_init();
    agent_init();
    proc_init();
    aih_init();
    agent_add("aih", AK_NATIVE, NULL, NULL, 0, &(const char *){ "" });
    linux_init();
    ui_init();
    gui_init(); mark(13,'R');
    evdev_init();
    fbdev_init();
    console_async_start(); mark(14,'S');
    bus_start();

    splash_info("Build",     fmt_build);
    splash_info("CPU",       fmt_cpu);
    splash_info("Memory",    fmt_mem);
    splash_info("Scheduler", fmt_sched);
    splash_info("Security",  fmt_sec);
    splash_info("Network",   fmt_net);
    splash_info("Modules",   fmt_mods);
    splash_show(); mark(15,'T');

    thread_create("init", 3, init_thread, 0);

    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");     /* idle thread */
}
