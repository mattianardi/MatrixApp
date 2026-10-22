#include "shell.h"
#include "net.h"
#include "fs.h"
#include "vfs.h"
#include "varbus.h"

static void err(const char *m) { kprintf("\x1b[91m%s\x1b[0m\n", m); }

int c_net(int argc, char **argv, struct ctx *c)
{
    if (argc > 1 && !strcmp(argv[1], "dhcp")) {
        if (c->pure) return -1;
        if (net_dhcp()) { err("DHCP failed (no NIC, no server, or no answer)"); return -1; }
        char b[24]; ip_format(net_ip(), b, sizeof b); kprintf("address %s\n", b);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "set")) {
        u32 ip, mask, gw, dns = 0;
        if (c->pure) return -1;
        if (argc < 5 || ip_parse(argv[2], &ip) || ip_parse(argv[3], &mask) || ip_parse(argv[4], &gw) || (argc > 5 && ip_parse(argv[5], &dns))) {
            err("usage: net set <ip> <mask> <gateway> [dns]"); return -1;
        }
        net_configure(ip, mask, gw, dns ? dns : gw);
        return 0;
    }
    static const char *keys[] = { "net.mac", "net.ip", "net.mask", "net.gateway", "net.dns" };
    tb_init(c->tb, "item", "value", NULL);
    for (int i = 0; i < 5; i++) {
        struct tunable *t = tune_find(keys[i]);
        if (!t || tb_newrow(c->tb) < 0) continue;
        tb_set(c->tb, 0, "%s", keys[i] + 4); tb_set(c->tb, 1, "%s", t->sval);
    }
    u64 rx, tx, drop; nic_stats(&rx, &tx, &drop);
    tb_newrow(c->tb); tb_set(c->tb, 0, "rx_frames"); tb_set(c->tb, 1, "%lu", rx);
    tb_newrow(c->tb); tb_set(c->tb, 0, "tx_frames"); tb_set(c->tb, 1, "%lu", tx);
    tb_newrow(c->tb); tb_set(c->tb, 0, "tx_dropped"); tb_set(c->tb, 1, "%lu", drop);
    return 0;
}

int c_ping(int argc, char **argv, struct ctx *c)
{
    u32 ip; i64 count = 4;
    if (c->pure) return -1;
    if (argc < 2) { err("usage: ping <host> [count]"); return -1; }
    if (argc > 2) parse_i64(argv[2], &count);
    if (net_resolve(argv[1], &ip)) { err("cannot resolve host"); return -1; }
    char b[24]; ip_format(ip, b, sizeof b);
    int lost = 0;
    for (i64 i = 0; i < count; i++) {
        u64 rtt;
        int r = net_ping(ip, 1000, &rtt);
        if (r) { kprintf("seq %ld: %s\n", i + 1, r == -1 ? "send failed (no route/ARP?)" : "timeout"); lost++; }
        else kprintf("reply from %s: seq %ld time %lu.%03lu ms\n", b, i + 1, rtt / 1000, rtt % 1000);
        if (i + 1 < count) thread_sleep_ms(300);
    }
    kprintf("%ld sent, %ld lost\n", count, (i64)lost);
    return lost == count ? -1 : 0;
}

int c_nslookup(int argc, char **argv, struct ctx *c)
{
    u32 ip;
    if (c->pure) return -1;
    if (argc < 2) { err("usage: nslookup <host>"); return -1; }
    if (net_resolve(argv[1], &ip)) { err("lookup failed"); return -1; }
    char b[24]; ip_format(ip, b, sizeof b);
    kprintf("%s -> %s\n", argv[1], b);
    return 0;
}

int c_pci(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "slot", "vendor", "device", "class", NULL);
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev d;
        if (pci_get(i, &d) || tb_newrow(c->tb) < 0) continue;
        tb_set(c->tb, 0, "%02x:%02x.%u", d.bus, d.dev, d.fn); tb_set(c->tb, 1, "0x%04x", d.vendor);
        tb_set(c->tb, 2, "0x%04x", d.device); tb_set(c->tb, 3, "0x%02x%02x", d.class, d.subclass);
    }
    return 0;
}

int c_http(int argc, char **argv, struct ctx *c)
{
    if (c->pure) return -1;
    if (argc < 2) { err("usage: http <url> [file]"); return -1; }
    size_t cap = 512 * 1024, n = 0; int st = 0; const char *e = "";
    u8 *buf = kmalloc(cap);
    if (!buf) { err("out of memory"); return -1; }
    if (http_get(argv[1], buf, cap, &n, &st, &e)) { kprintf("\x1b[91mhttp: %s\x1b[0m\n", e); kfree(buf); return -1; }
    kprintf("HTTP %d, %lu bytes\n", st, (u64)n);
    if (argc > 2) {
        char p[128]; fs_norm(argv[2], p, sizeof p);
        if (fs_write(p, buf, n)) err("cannot write file"); else kprintf("saved to %s\n", p);
    } else {
        for (size_t i = 0; i < n && i < 1024; i++) kputc(buf[i] == '\n' || (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.');
        if (n > 1024) kprintf("\n... (%lu more bytes)", (u64)(n - 1024));
        kputc('\n');
    }
    kfree(buf);
    return 0;
}

int c_ls(int argc, char **argv, struct ctx *c)
{
    char dir[128], p[128]; size_t sz;
    fs_norm(argc > 1 ? argv[1] : ".", dir, sizeof dir);
    size_t dl = strlen(dir); if (dl == 1) dl = 0;
    tb_init(c->tb, "name", "type", "size", NULL);
    int any = 0;
    for (int i = 0; !fs_info(i, p, sizeof p, &sz); i++) {
        if (strncmp(p, dir, dl) || p[dl] != '/') continue;
        any = 1;
        char *rest = p + dl + 1, *slash = strchr(rest, '/');
        char name[40]; size_t nl = slash ? (size_t)(slash - rest) : strlen(rest);
        if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, rest, nl); name[nl] = 0;
        int dup = 0;
        for (int r = 0; r < c->tb->nrows; r++) if (!strcmp(c->tb->cell[r][0], name)) dup = 1;
        if (dup || tb_newrow(c->tb) < 0) continue;
        tb_set(c->tb, 0, "%s", name); tb_set(c->tb, 1, "%s", slash ? "dir" : "file"); tb_set(c->tb, 2, "%lu", slash ? 0ul : (u64)sz);
    }
    struct inode *di = vfs_lookup(dir);
    if (!any && dl && !(di && (di->mode & S_IFMT) == S_IFDIR)) { c->tb->ncols = 0; if (!c->pure) err("no such directory"); return -1; }
    return 0;
}

int c_cat(int argc, char **argv, struct ctx *c)
{
    char p[128]; const u8 *d; size_t n;
    if (c->pure) return -1;
    if (argc < 2) { err("usage: cat <file>"); return -1; }
    fs_norm(argv[1], p, sizeof p);
    if (fs_read(p, &d, &n)) { err("no such file"); return -1; }
    for (size_t i = 0; i < n && i < 4096; i++) kputc(d[i] == '\n' || (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.');
    if (n && d[n - 1] != '\n') kputc('\n');
    return 0;
}

int c_rm(int argc, char **argv, struct ctx *c)
{
    char p[128];
    if (c->pure) return -1;
    if (argc < 2) { err("usage: rm <file>"); return -1; }
    fs_norm(argv[1], p, sizeof p);
    if (fs_remove(p)) { err("no such file"); return -1; }
    return 0;
}

int c_write(int argc, char **argv, struct ctx *c)
{
    char p[128], text[200] = "";
    if (c->pure) return -1;
    if (argc < 3) { err("usage: write <file> <text>"); return -1; }
    for (int i = 2; i < argc; i++) { if (i > 2) strlcat(text, " ", sizeof text); strlcat(text, argv[i], sizeof text); }
    strlcat(text, "\n", sizeof text);
    fs_norm(argv[1], p, sizeof p);
    if (fs_write(p, text, strlen(text))) { err("cannot write file"); return -1; }
    return 0;
}

int c_cd(int argc, char **argv, struct ctx *c)
{
    char p[128], q[128]; size_t sz; const char *m = "";
    if (c->pure) return -1;
    fs_norm(argc > 1 ? argv[1] : "/", p, sizeof p);
    int ok = !strcmp(p, "/");
    size_t pl = strlen(p);
    for (int i = 0; !ok && !fs_info(i, q, sizeof q, &sz); i++) if (!strncmp(q, p, pl) && q[pl] == '/') ok = 1;
    if (!ok) { err("no such directory"); return -1; }
    if (bus_set("PWD", p, NEOS_VIEW_LINUX, 1, &m)) { err(m); return -1; }
    return 0;
}

int c_pwd(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv; (void)c;
    char l[128], w[128];
    bus_get("PWD", NEOS_VIEW_LINUX, l, sizeof l); bus_get("PWD", NEOS_VIEW_WINDOWS, w, sizeof w);
    kprintf("%s   (%s)\n", l, w);
    return 0;
}

static int fg_pid;
static int ctrl_c(void);
#include "proc.h"
#include "usb.h"
/* Package backends live in /lib/neos/pkg/<name>.sh (shipped in the initrd): `pkg install pip:requests`, `deb:<file|url>`, `tar:<url>`, `apt:<name>`.
 * They run as ordinary Linux programs; the registry (/var/pkg, /opt, PATH) stays the one place that says what is installed. */
static int run_backend(const char *backend, const char *sub, const char *spec)
{
    char script[64]; ksnprintf(script, sizeof script, "/lib/neos/pkg/%s.sh", backend);
    if (!vfs_lookup(script)) { kprintf("\x1b[91mpkg: no backend '%s'\x1b[0m\n", backend); return -1; }
    char *av[5] = { "/bin/sh", script, (char *)sub, (char *)spec, NULL };
    struct spawn_opts o = { 0 }; int pid; const char *e = "";
    o.grant[0] = "pkg:*"; o.ngrant = 1;                                  /* the operator typed `pkg ...`: the backend may register what it installs */
    if (proc_spawn("/bin/sh", spec ? 4 : 3, av, &o, &pid, &e)) { kprintf("\x1b[91mpkg: %s\x1b[0m\n", e); return -1; }
    i64 code; fg_pid = pid;
    int r = proc_wait(pid, &code, ctrl_c);
    fg_pid = 0;
    if (r == -2) { proc_kill(pid, 130); kprintf("^C\n"); return -1; }
    return code == 0 ? 0 : -1;
}

int c_pkg(int argc, char **argv, struct ctx *c)
{
    const char *sub = argc > 1 ? argv[1] : "list", *e = "";
    if (!strcmp(sub, "list")) { pkg_table(c->tb); return 0; }
    if (!strcmp(sub, "installed")) { pkg_installed_table(c->tb); return 0; }
    if (c->pure) return -1;
    int r;
    if ((!strcmp(sub, "install") || !strcmp(sub, "remove")) && argc > 2) {
        const char *colon = strchr(argv[2], ':');
        if (colon && colon > argv[2] && colon - argv[2] < 8 && colon[1] != '/') {               /* backend:spec */
            char be[10]; size_t bl = (size_t)(colon - argv[2]); memcpy(be, argv[2], bl); be[bl] = 0;
            return run_backend(be, sub, colon + 1);
        }
    }
    if (!strcmp(sub, "update")) r = pkg_update(&e);
    else if (!strcmp(sub, "info") && argc > 2) { if (pkg_info(argv[2])) { err("unknown package"); return -1; } return 0; }
    else if (!strcmp(sub, "install") && argc > 2) r = pkg_install(argv[2], &e);
    else if (!strcmp(sub, "remove") && argc > 2) r = pkg_remove(argv[2], &e);
    else { err("usage: pkg [update | list | installed | info <n> | install <n | pip:x | deb:file | tar:url | apt:n> | remove <n | pip:x ...>]"); return -1; }
    if (r) { kprintf("\x1b[91mpkg: %s\x1b[0m\n", e); return -1; }
    return 0;
}

int c_sha256(int argc, char **argv, struct ctx *c)
{
    char p[128], h[65]; const u8 *d; size_t n;
    if (c->pure) return -1;
    if (argc < 2) { err("usage: sha256 <file>"); return -1; }
    fs_norm(argv[1], p, sizeof p);
    if (fs_read(p, &d, &n)) { err("no such file"); return -1; }
    sha256_hex(d, n, h);
    kprintf("%s  %s (%lu bytes)\n", h, p, (u64)n);
    return 0;
}

/* ---- processes ---- */
#include "proc.h"
#include "agent.h"
#include "fdt.h"
#include "aih.h"

static int fg_pid;
int tty_pump(void)
{
    int k, got = 0;
    if (!fg_pid) return 0;                                      /* keys belong to the shell unless a foreground program is running */
    while ((k = key_poll()) >= 0) { got = 1; if (k == 3 && fg_pid) { proc_kill(fg_pid, 130); kprintf("^C\n"); } else if (k == 20) sched_dump(); else tty_feed(k); }
    return got;
}
static int ctrl_c(void) { tty_pump(); return 0; }

int c_run(int argc, char **argv, struct ctx *c)
{
    struct spawn_opts o = { 0 };
    int bg = 0, i = 1;
    if (c->pure) return -1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-b")) bg = 1;
        else if (!strcmp(argv[i], "-g") && i + 1 < argc && o.ngrant < 8) o.grant[o.ngrant++] = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) o.world = strcmp(argv[++i], "win") ? NEOS_VIEW_LINUX : NEOS_VIEW_WINDOWS;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) { i64 v; if (parse_i64(argv[++i], &v) || v < 0 || v > 7) { err("priority 0-7"); return -1; } o.prio = (int)v + 1; }
        else { err("usage: run [-b] [-g cap[:scope]]... [-w lin|win] [-p prio] <program> [args...]"); return -1; }
    }
    if (i >= argc) { err("usage: run [-b] [-g cap[:scope]]... [-w lin|win] [-p prio] <program> [args...]"); return -1; }
    int pid; const char *e = "";
    if (proc_spawn(argv[i], argc - i, argv + i, &o, &pid, &e)) { kprintf("\x1b[91mrun: %s\x1b[0m\n", e); return -1; }
    if (bg) { kprintf("[pid %d] started in background\n", pid); return 0; }
    i64 code;
    fg_pid = pid;
    int r = proc_wait(pid, &code, ctrl_c);
    fg_pid = 0;
    if (r == -2) { proc_kill(pid, 130); kprintf("^C\n[pid %d] interrupted\n", pid); return -1; }
    kprintf("[pid %d] exited with code %ld\n", pid, code);
    return code == 0 ? 0 : -1;
}

int c_proc(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    static const char *st[] = { "-", "starting", "running", "zombie", "exited" };
    tb_init(c->tb, "pid", "name", "state", "pages", "syscalls", "age_ms", "exit", NULL);
    for (int s = 0; s < MAX_PROCS; s++) {
        struct proc *p = proc_get(s);
        if (!p || tb_newrow(c->tb) < 0) continue;
        tb_set(c->tb, 0, "%d", p->pid); tb_set(c->tb, 1, "%s", p->name); tb_set(c->tb, 2, "%s", st[p->state]);
        tb_set(c->tb, 3, "%lu", (u64)p->as.pages); tb_set(c->tb, 4, "%lu", p->syscalls);
        tb_set(c->tb, 5, "%lu", (ticks - p->start_tick) * 1000 / timer_hz());
        if (p->state >= P_ZOMBIE) tb_set(c->tb, 6, "%ld", p->exit_code); else tb_set(c->tb, 6, "-");
    }
    return 0;
}

int c_pkill(int argc, char **argv, struct ctx *c)
{
    i64 pid;
    if (c->pure) return -1;
    if (argc < 2 || parse_i64(argv[1], &pid) || proc_kill((int)pid, 137)) { err("usage: pkill <pid>   (see `proc`)"); return -1; }
    return 0;
}

int c_binfmt(int argc, char **argv, struct ctx *c)
{
    if (argc >= 3 && !c->pure) {
        if (!strcmp(argv[1], "add") && argc >= 4) { if (binfmt_add(argv[2], argv[3], argc > 4 ? argv[4] : "")) { err("binfmt table full"); return -1; } return 0; }
        if (!strcmp(argv[1], "remove")) { if (binfmt_remove(argv[2])) { err("no such extension"); return -1; } return 0; }
    }
    tb_init(c->tb, "extension", "interpreter", "arg", NULL);
    char ext[12], interp[48], arg[24];
    for (int i = 0; !binfmt_get(i, ext, interp, arg); i++) { if (tb_newrow(c->tb) < 0) break; tb_set(c->tb, 0, "%s", ext); tb_set(c->tb, 1, "%s", interp); tb_set(c->tb, 2, "%s", arg); }
    return 0;
}

/* `lx <cmd>`: force the Linux program even when NEOS has a built-in of the same name */
int c_lx(int argc, char **argv, struct ctx *c)
{
    if (argc < 2) { err("usage: lx <linux-command> [args]   (runs /bin/<command>)"); return -1; }
    char path[128];
    ksnprintf(path, sizeof path, "/bin/%s", argv[1]);
    char *av[16]; int n = 0;
    av[n++] = "run"; av[n++] = path;
    for (int i = 2; i < argc && n < 15; i++) av[n++] = argv[i];
    av[n] = NULL;
    return c_run(n, av, c);
}

/* unknown command: is it a program? search $PATH (Linux form), then the cwd for scripts */
int shell_try_external(int argc, char **argv, struct ctx *c)
{
    char found[128] = "", pathv[256] = "/bin:/usr/bin", cand[160];
    bus_get("PATH", NEOS_VIEW_LINUX, pathv, sizeof pathv);
    if (strchr(argv[0], '/') || strchr(argv[0], '\\')) { fs_norm(argv[0], cand, sizeof cand); struct inode *i = vfs_lookup(cand); if (i && (i->mode & S_IFMT) != S_IFDIR) strlcpy(found, cand, sizeof found); }
    else {
        for (char *d = pathv; *d;) {
            char *e2 = strchr(d, ':'); size_t dl = e2 ? (size_t)(e2 - d) : strlen(d);
            if (dl && dl < 100) { char dir[100]; memcpy(dir, d, dl); dir[dl] = 0; ksnprintf(cand, sizeof cand, "%s/%s", dir, argv[0]); struct inode *i = vfs_lookup(cand); if (i && (i->mode & S_IFMT) != S_IFDIR) { strlcpy(found, cand, sizeof found); break; } }
            if (!e2) break; d = e2 + 1;
        }
        if (!found[0]) { fs_norm(argv[0], cand, sizeof cand); struct inode *i = vfs_lookup(cand); if (i && (i->mode & S_IFMT) == S_IFREG) strlcpy(found, cand, sizeof found); }
    }
    if (!found[0]) return -1;
    char *av[16]; int n = 0;
    av[n++] = "run"; av[n++] = found;
    for (int i = 1; i < argc && n < 15; i++) av[n++] = argv[i];
    av[n] = NULL;
    c_run(n, av, c);
    return 0;
}

/* ------------------------------------------------------------ aih */
static int ask_line(const char *prompt, char *buf, size_t n)
{
    size_t l = 0;
    kprintf("%s", prompt);
    for (;;) {
        int k = key_poll();
        if (k < 0) { thread_sleep_ms(5); continue; }
        if (k == '\n') { kputc('\n'); buf[l] = 0; return (int)l; }
        if (k == 3) { kprintf("^C\n"); buf[0] = 0; return -1; }
        if ((k == '\b' || k == 127) && l) { l--; kprintf("\b \b"); }
        else if (k >= 32 && k < 127 && l + 1 < n) { buf[l++] = (char)k; kputc((char)k); }
    }
}

static void aih_exec_step(const char *world, const char *cmd)
{
    struct ctx xc = { NULL, "", 0 };
    if (!strcmp(world, "neos")) { shell_run_line(cmd); return; }
    char *av[8]; int n = 0;
    av[n++] = "run";
    if (!strcmp(world, "linux")) { av[n++] = "/bin/busybox"; av[n++] = "sh"; av[n++] = "-c"; av[n++] = (char *)cmd; }
    else if (!strcmp(world, "nio")) { av[n++] = "/bin/nio"; av[n++] = "-e"; av[n++] = (char *)cmd; }
    else if (!strcmp(world, "windows")) {
        if (!vfs_lookup("/bin/winrt")) { kprintf("\x1b[93m  (Windows programs need the PE layer /bin/winrt, which is not installed)\x1b[0m\n"); return; }
        av[n++] = "-w"; av[n++] = "win"; av[n++] = "/bin/winrt"; av[n++] = (char *)cmd;
    } else { kprintf("\x1b[91m  unknown world '%s'\x1b[0m\n", world); return; }
    av[n] = NULL;
    c_run(n, av, &xc);
}

static int c_aih_inner(int argc, char **argv, struct ctx *c);
int c_aih(int argc, char **argv, struct ctx *c)
{
    if (c->pure) return -1;
    int r = c_aih_inner(argc, argv, c);
    c->tb->ncols = c->tb->nrows = c->tb->more = 0;                 /* steps ran nested shell lines that used the same result table */
    return r;
}

static int c_aih_inner(int argc, char **argv, struct ctx *c)
{
    (void)argv;
    if (argc < 2 || !c->raw[0]) {
        kprintf("AIH - AI Help. Say what you want to do, in Italian or English:\n  aih mostra la memoria        aih install hello        aih imposta il quantum a 5\n"
                "  aih dir                     (a Windows/Linux command is translated)    aih ottimizza il sistema\n");
        return 0;
    }
    static char plan[AIH_PLAN_MAX], from[16];
    int attached = aih_planner_active();
    if (attached) kprintf("\x1b[90m(asking the attached AI planner, up to %d s...)\x1b[0m\n", (int)tune_find("aih.timeout_s")->val);
    aih_request(c->raw, plan, sizeof plan, from, sizeof from);

    char says[400] = ""; struct { char world[10], risk[8], cmd[200]; int ok; } st[12]; int ns = 0;
    for (char *l = plan; *l;) {
        char *e2 = strchr(l, '\n'); size_t ll = e2 ? (size_t)(e2 - l) : strlen(l);
        char line[300]; if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, l, ll); line[ll] = 0;
        if (!strncmp(line, "SAY ", 4)) { if (says[0]) strlcat(says, " ", sizeof says); strlcat(says, line + 4, sizeof says); }
        else if (!strncmp(line, "STEP ", 5) && ns < 12) {
            char *w = line + 5, *r = strchr(w, ' '); if (!r) goto next; *r++ = 0;
            char *cm = strchr(r, ' '); if (!cm) goto next; *cm++ = 0;
            strlcpy(st[ns].world, w, sizeof st[ns].world); strlcpy(st[ns].risk, r, sizeof st[ns].risk); strlcpy(st[ns].cmd, cm, sizeof st[ns].cmd);
            ns++;
        }
      next:
        if (!e2) break;
        l = e2 + 1;
    }
    kprintf("\x1b[1;96maih\x1b[0m \x1b[90m[planner: %s]\x1b[0m %s\n", from, says[0] ? says : "(no explanation)");
    int all_low = 1;
    for (int i = 0; i < ns; i++) {
        int hi = !strcmp(st[i].risk, "high"), md = !strcmp(st[i].risk, "medium");
        if (hi || md || strcmp(st[i].risk, "low")) all_low = 0;
        kprintf("  %d. %s[%-6s]\x1b[0m %-8s %s\n", i + 1, hi ? "\x1b[91m" : md ? "\x1b[93m" : "\x1b[92m", st[i].risk, st[i].world, st[i].cmd);
    }
    struct agent *ag = agent_find("aih");
    if (ag) audit_add(agent_index(ag), "aih.plan", c->raw, ns, 1);
    if (!ns) return 0;
    char ans[16];
    if (!(all_low && aih_autorun_low())) {
        char prompt[64]; ksnprintf(prompt, sizeof prompt, "Run %d step%s? [y/N] ", ns, ns == 1 ? "" : "s");
        if (ask_line(prompt, ans, sizeof ans) < 0 || (ans[0] != 'y' && ans[0] != 'Y' && ans[0] != 's' && ans[0] != 'S')) { kprintf("cancelled, nothing was run.\n"); return 0; }
    }
    for (int i = 0; i < ns; i++) {
        if (!strcmp(st[i].risk, "high")) {
            char p2[64]; ksnprintf(p2, sizeof p2, "step %d is HIGH risk (%.40s): type YES to run it: ", i + 1, st[i].cmd);
            if (ask_line(p2, ans, sizeof ans) < 0 || strcmp(ans, "YES")) { kprintf("step %d skipped.\n", i + 1); continue; }
        }
        kprintf("\x1b[90m> %s\x1b[0m\n", st[i].cmd);
        aih_exec_step(st[i].world, st[i].cmd);
    }
    if (ag) audit_add(agent_index(ag), "aih.run", c->raw, 0, 1);
    return 0;
}

int c_disk(int argc, char **argv, struct ctx *c)
{
    (void)c;
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "status")) { neofs_status(); return 0; }
    if (!strcmp(sub, "sync")) {
        int r = neofs_sync(1);
        if (r) { kprintf("\x1b[91mdisk: sync failed (%d)%s\x1b[0m\n", r, neofs_present() ? "" : " - no disk attached"); return -1; }
        kprintf("snapshot written\n"); return 0;
    }
    if (!strcmp(sub, "format")) { if (neofs_format()) { kprintf("\x1b[91mdisk: no disk\x1b[0m\n"); return -1; } kprintf("disk snapshots erased (the running system is untouched)\n"); return 0; }
    kprintf("usage: disk [status|sync|format]\n");
    return -1;
}

int c_browse(int argc, char **argv, struct ctx *c)
{
    (void)c;
    if (!gui_active()) { kprintf("\x1b[91mbrowse: the graphical workspace is not running\x1b[0m\n"); return -1; }
    gui_browse(argc > 1 ? argv[1] : "neos://home");
    return 0;
}

int c_fontdump(int argc, char **argv, struct ctx *c) { (void)argc; (void)argv; (void)c; extern void gui_fontdump(void); gui_fontdump(); return 0; }

/* Familiar front-ends: `pip install x`, `apt install x`, `dpkg -i f.deb` are the same registry operations as `pkg install <backend>:x`. */
static int join_args(char *out, size_t n, int argc, char **argv, int from)
{
    out[0] = 0;
    for (int i = from; i < argc; i++) { if (out[0]) strlcat(out, " ", n); strlcat(out, argv[i], n); }
    return out[0] != 0;
}
int c_pip(int argc, char **argv, struct ctx *c)
{
    (void)c; char spec[200];
    if (c->pure) return -1;
    if (argc < 2) { err("usage: pip install|uninstall|list|show|freeze ... (python3 comes from the package cache on first use)"); return -1; }
    if (!strcmp(argv[1], "install") && join_args(spec, sizeof spec, argc, argv, 2)) return run_backend("pip", "install", spec);
    if (!strcmp(argv[1], "uninstall") && join_args(spec, sizeof spec, argc, argv, 2)) { char *a2[2]; (void)a2; return run_backend("pip", "remove", spec); }
    join_args(spec, sizeof spec, argc, argv, 1);
    return run_backend("pip", "run", spec);
}
int c_apt(int argc, char **argv, struct ctx *c)
{
    char spec[200];
    if (c->pure) return -1;
    if (argc < 2) { err("usage: apt update | install <pkg>... | remove <pkg> | search <text> | show <pkg>"); return -1; }
    if (!strcmp(argv[1], "update")) return run_backend("apt", "install", "update");
    if ((!strcmp(argv[1], "install") || !strcmp(argv[1], "remove") || !strcmp(argv[1], "search") || !strcmp(argv[1], "show")) && join_args(spec, sizeof spec, argc, argv, 2))
        return run_backend("apt", argv[1], spec);
    err("usage: apt update | install <pkg>... | remove <pkg> | search <text> | show <pkg>");
    return -1;
}
int c_dpkg(int argc, char **argv, struct ctx *c)
{
    if (c->pure) return -1;
    if (argc >= 3 && (!strcmp(argv[1], "-i") || !strcmp(argv[1], "--install"))) return run_backend("deb", "install", argv[2]);
    if (argc >= 3 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "--remove"))) return run_backend("deb", "remove", argv[2]);
    err("usage: dpkg -i <file.deb> | dpkg -r <name>");
    return -1;
}

int c_usb(int argc, char **argv, struct ctx *c)
{
    const char *sub = argc > 1 ? argv[1] : "list", *e = "";
    if (!strcmp(sub, "list")) {
        tb_init(c->tb, "#", "port", "device", "class", NULL);
        if (!usb_present()) return 0;
        for (int i = 0; i < usb_count(); i++) {
            const struct usb_dev *d = usb_get(i); if (!d || tb_newrow(c->tb) < 0) continue;
            tb_set(c->tb, 0, "%d", i); tb_set(c->tb, 1, "%d", d->port); tb_set(c->tb, 2, "%s", d->name); tb_set(c->tb, 3, "%02x", d->cls);
        }
        return 0;
    }
    if (c->pure) return -1;
    if (!strcmp(sub, "mount")) { if (fat_mount("/mnt/usb", &e)) { kprintf("\x1b[91musb: %s\x1b[0m\n", e); return -1; } return 0; }
    if (!strcmp(sub, "sync")) { if (fat_sync(&e)) { kprintf("\x1b[91musb: %s\x1b[0m\n", e); return -1; } return 0; }
    if (!strcmp(sub, "umount")) { if (fat_sync(&e)) { kprintf("\x1b[91musb: %s\x1b[0m\n", e); return -1; } fat_umount(); kprintf("/mnt/usb unmounted: it is safe to remove the stick\n"); return 0; }
    err("usage: usb [list | mount | sync | umount]");
    return -1;
}
