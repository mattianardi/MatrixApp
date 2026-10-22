#include "shell.h"
#include "net.h"
#include "fs.h"
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
    u64 rx, tx, drop; e1000_stats(&rx, &tx, &drop);
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
    if (!any && dl) { c->tb->ncols = 0; if (!c->pure) err("no such directory (the filesystem is in RAM and starts empty)"); return -1; }
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

int c_pkg(int argc, char **argv, struct ctx *c)
{
    const char *sub = argc > 1 ? argv[1] : "list", *e = "";
    if (!strcmp(sub, "list")) { pkg_table(c->tb); return 0; }
    if (c->pure) return -1;
    int r;
    if (!strcmp(sub, "update")) r = pkg_update(&e);
    else if (!strcmp(sub, "info") && argc > 2) { if (pkg_info(argv[2])) { err("unknown package"); return -1; } return 0; }
    else if (!strcmp(sub, "install") && argc > 2) r = pkg_install(argv[2], &e);
    else if (!strcmp(sub, "remove") && argc > 2) r = pkg_remove(argv[2], &e);
    else { err("usage: pkg [update | list | info <n> | install <n> | remove <n>]"); return -1; }
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
