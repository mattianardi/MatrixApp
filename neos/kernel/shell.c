#include "shell.h"
#include "varbus.h"

/*
 * NEOS shell: the single front end. Commands return structured tables, not
 * text, so the same command can be rendered, filtered (`| where | sort | head
 * | cols | count | json`), or pinned to the live dock (`| pin`). Variables are
 * expanded in both conventions ($HOME, ${PATH@win}, %USERPROFILE%) in one line.
 */
typedef int (*cmd_fn)(int argc, char **argv, struct ctx *c);
struct cmd { const char *name, *usage, *help; cmd_fn fn; u8 pure; u8 complete; };   /* complete: 1 = tunables, 2 = vars */

#define LINE 256
static struct table out_tb;
static char hist[16][LINE];
static int nhist;
static struct tunable *t_prio;
static const char PROMPT[] = "\x1b[1;36mneos\x1b[0m> ";

static void err(const char *m) { kprintf("\x1b[91m%s\x1b[0m\n", m); }

/* ------------------------------------------------------------ parsing */
static int tokenize(char *s, char **argv, int max)
{
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ') s++;
        if (!*s) break;
        argv[n++] = s;
        char *w = s, q = 0;
        while (*s && (q || *s != ' ')) {
            if (q) { if (*s == q) { q = 0; s++; } else *w++ = *s++; }
            else if (*s == '"' || *s == '\'') { q = *s; s++; }
            else *w++ = *s++;
        }
        if (*s) s++;
        *w = 0;
    }
    argv[n] = NULL;
    return n;
}

/* expand variables, leaving '...' spans untouched */
static void expand_line(const char *in, char *out, size_t n)
{
    char seg[LINE], tmp[LINE * 2];
    size_t l = 0, sl = 0;
    #define FLUSH() do { seg[sl] = 0; bus_expand(seg, tmp, sizeof tmp); \
        for (size_t i_ = 0; tmp[i_] && l + 1 < n; i_++) { out[l++] = tmp[i_]; } sl = 0; } while (0)
    while (*in) {
        if (*in == '\'') {
            FLUSH();
            out[l++] = *in++;
            while (*in && *in != '\'' && l + 1 < n) out[l++] = *in++;
            if (*in) out[l++] = *in++;
        } else if (sl + 1 < sizeof seg) seg[sl++] = *in++;
        else in++;
    }
    FLUSH();
    out[l] = 0;
    #undef FLUSH
}

static int split_stages(char *s, char **st, int max)
{
    int n = 0; char q = 0;
    st[n++] = s;
    for (; *s; s++) {
        if (q) { if (*s == q) q = 0; }
        else if (*s == '"' || *s == '\'') q = *s;
        else if (*s == '|' && n < max) { *s = 0; st[n++] = s + 1; }
    }
    return n;
}

static char *trim(char *s)
{
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = 0;
    return s;
}

/* ------------------------------------------------------------ commands */
static int c_help(int argc, char **argv, struct ctx *c);

static int c_clear(int argc, char **argv, struct ctx *c) { (void)argc; (void)argv; (void)c; console_clear_stream(); return 0; }
static int c_echo(int argc, char **argv, struct ctx *c)
{
    (void)c;
    for (int i = 1; i < argc; i++) kprintf("%s%s", i > 1 ? " " : "", argv[i]);
    kprintf("\n");
    return 0;
}
static int c_about(int argc, char **argv, struct ctx *c) { (void)argc; (void)argv; (void)c; splash_show(); return 0; }

static int c_uptime(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv; (void)c;
    u64 ms = time_ns() / 1000000;
    kprintf("up %lu.%03lu s, %lu ticks @ %u Hz\n", ms / 1000, ms % 1000, (u64)ticks, timer_hz());
    return 0;
}

static int c_ps(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "id", "name", "prio", "state", "cpu%", "ticks", "stack_kib", NULL);
    u64 total = sched_runticks_total() ? sched_runticks_total() : 1;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        struct thread_info ti;
        if (thread_info(i, &ti) || tb_newrow(c->tb) < 0) continue;
        tb_set(c->tb, 0, "%d", ti.id); tb_set(c->tb, 1, "%s", ti.name); tb_set(c->tb, 2, "%d", ti.prio);
        tb_set(c->tb, 3, "%s", ti.state);
        u64 p = ti.runticks * 1000 / total;
        tb_set(c->tb, 4, "%lu.%lu", p / 10, p % 10); tb_set(c->tb, 5, "%lu", ti.runticks); tb_set(c->tb, 6, "%d", ti.stack_kib);
    }
    return 0;
}

static int c_mods(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "module", "version", "tunables", "description", NULL);
    for (int i = 0; i < module_count(); i++) {
        const char *n, *v, *d; module_get(i, &n, &v, &d);
        int cnt = 0;
        for (int k = 0; k < tune_count(); k++) if (!strcmp(tune_at(k)->module, n)) cnt++;
        if (tb_newrow(c->tb) < 0) break;
        tb_set(c->tb, 0, "%s", n); tb_set(c->tb, 1, "%s", v); tb_set(c->tb, 2, "%d", cnt); tb_set(c->tb, 3, "%s", d);
    }
    return 0;
}

static int c_tune(int argc, char **argv, struct ctx *c)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (!strcmp(sub, "list") || !strcmp(sub, "pending")) {
        int pend = !strcmp(sub, "pending");
        const char *filt = argc > 2 ? argv[2] : "";
        tb_init(c->tb, "name", "value", "default", "mode", "description", NULL);
        for (int i = 0; i < tune_count(); i++) {
            struct tunable *t = tune_at(i); char v[64], d[64];
            if (pend && !t->has_pending) continue;
            if (strncmp(t->name, filt, strlen(filt))) continue;
            if (tb_newrow(c->tb) < 0) break;
            tune_format(t, v, sizeof v);
            tune_format(&(struct tunable){ .type = t->type, .val = t->def, .sval = "" }, d, sizeof d);
            if (t->type == TT_STR) strlcpy(d, "", sizeof d);
            tb_set(c->tb, 0, "%s", t->name); tb_set(c->tb, 1, "%s", pend ? (t->type == TT_STR ? t->spend : v) : v);
            tb_set(c->tb, 2, "%s", d);
            tb_set(c->tb, 3, "%s", t->flags & TF_RO ? "ro" : t->flags & TF_BOOT ? "boot" : "live");
            tb_set(c->tb, 4, "%s", t->desc);
        }
        return 0;
    }
    if (c->pure) return -1;
    if (!strcmp(sub, "get") && argc > 2) {
        struct tunable *t = tune_find(argv[2]); char v[64];
        if (!t) { err("no such setting"); return -1; }
        tune_format(t, v, sizeof v);
        kprintf("%s = %s   (%s)\n", t->name, v, t->desc);
        return 0;
    }
    if (!strcmp(sub, "set") && argc > 3) {
        const char *m = ""; int r = tune_set(argv[2], argv[3], &m);
        if (r < 0) { kprintf("\x1b[91mtune: %s: %s\x1b[0m\n", argv[2], m); return -1; }
        kprintf("%s = %s  \x1b[90m(%s)\x1b[0m\n", argv[2], argv[3], r ? m : "applied");
        return 0;
    }
    if (!strcmp(sub, "export")) {
        static char buf[2048];
        tune_save_text(buf, sizeof buf);
        kprintf("# kernel command line fragment (boot-time settings):\n");
        for (char *l = buf, *e; *l; l = e + 1) { e = strchr(l, '\n'); if (!e) break; *e = 0; kprintf("neos.%s\n", l); }
        return 0;
    }
    err("usage: tune [list [prefix] | pending | get <name> | set <name> <value> | export]");
    return -1;
}

static int c_mem(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv; (void)c;
    size_t u, p, t; heap_stats(&u, &p, &t);
    kprintf("physical : %lu / %lu KiB free (4 KiB pages)\n", (u64)pmm_free_pages() * 4, (u64)pmm_total_pages() * 4);
    kprintf("kheap    : %lu KiB used, %lu KiB peak, %lu KiB arena\n", (u64)u >> 10, (u64)p >> 10, (u64)t >> 10);
    kprintf("console  : %lu bytes dropped\n", console_dropped());
    return 0;
}

static int c_rt(int argc, char **argv, struct ctx *c)
{
    (void)c;
    if (argc > 1 && !strcmp(argv[1], "reset")) { timer_jitter_reset(); kprintf("jitter statistics reset\n"); return 0; }
    u64 mn, mx, avg, n;
    timer_jitter(&mn, &mx, &avg, &n);
    kprintf("tick period: min %lu ns, avg %lu ns, max %lu ns  (%lu samples, nominal %lu ns)\n",
            mn, avg, mx, n, 1000000000ull / timer_hz());
    kprintf("jitter (max - min): %lu ns   TSC %lu MHz %s\n", mx - mn, tsc_hz() / 1000000,
            cpu_invariant_tsc() ? "(invariant)" : "(NOT invariant: not suitable as a time base)");
    return 0;
}

static int c_selftest(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    if (c->pure) return -1;
    kprintf("running RT self-test (~0.5 s)...\n");
    return selftest_run();
}

static int c_renice(int argc, char **argv, struct ctx *c)
{
    i64 id, pr;
    if (c->pure) return -1;
    if (argc < 3 || parse_i64(argv[1], &id) || parse_i64(argv[2], &pr)) { err("usage: renice <id> <prio 0-7>"); return -1; }
    if (thread_set_prio((int)id, (int)pr)) { err("renice: bad thread or priority"); return -1; }
    return 0;
}

static int c_kill(int argc, char **argv, struct ctx *c)
{
    i64 id;
    if (c->pure) return -1;
    if (argc < 2 || parse_i64(argv[1], &id)) { err("usage: kill <id>"); return -1; }
    if (thread_kill((int)id)) { err("kill: no such thread"); return -1; }
    return 0;
}

static int c_sleep(int argc, char **argv, struct ctx *c)
{
    i64 ms;
    if (c->pure) return -1;
    if (argc < 2 || parse_i64(argv[1], &ms) || ms < 0) { err("usage: sleep <ms>"); return -1; }
    thread_sleep_ms((u64)ms);
    return 0;
}

/* ---- variables ---- */
static int view_from(const char *s)
{
    if (!s) return NEOS_VIEW_LINUX;
    if (!strcmp(s, "win") || !strcmp(s, "windows") || !strcmp(s, "--as=win")) return NEOS_VIEW_WINDOWS;
    if (!strcmp(s, "raw")) return NEOS_VIEW_NATIVE;
    return NEOS_VIEW_LINUX;
}

static int assign(const char *name, const char *val, int view)
{
    const char *m = "";
    if (bus_set(name, val, view, 1, &m)) { kprintf("\x1b[91m%s: %s\x1b[0m\n", name, m); return -1; }
    return 0;
}

static int c_vars(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "name", "type", "linux", "windows", "set_by", "ver", NULL);
    for (int i = 0; i < bus_count(); i++) {
        struct bus_var_info vi;
        if (bus_info(i, &vi) || tb_newrow(c->tb) < 0) continue;
        char l[TB_CELL], w[TB_CELL];
        bus_get(vi.name, NEOS_VIEW_LINUX, l, sizeof l); bus_get(vi.name, NEOS_VIEW_WINDOWS, w, sizeof w);
        tb_set(c->tb, 0, "%s", vi.name); tb_set(c->tb, 1, "%s", bus_type_name(vi.type));
        tb_set(c->tb, 2, "%s", l); tb_set(c->tb, 3, "%s", w);
        tb_set(c->tb, 4, "%s", vi.view == NEOS_VIEW_WINDOWS ? "windows" : vi.view == NEOS_VIEW_LINUX ? "linux" : "neos");
        tb_set(c->tb, 5, "%u", vi.ver);
    }
    return 0;
}

static int c_var(int argc, char **argv, struct ctx *c)
{
    if (argc == 1) return c_vars(argc, argv, c);
    if (!strcmp(argv[1], "set")) {
        if (c->pure) return -1;
        if (argc < 4) { err("usage: var set <name> <value> [lin|win|raw]"); return -1; }
        return assign(argv[2], argv[3], view_from(argc > 4 ? argv[4] : NULL));
    }
    if (!strcmp(argv[1], "type") && argc > 3) {
        if (c->pure) return -1;
        const char *names[] = { "str", "int", "bool", "path", "pathlist" };
        for (int t = 0; t < 5; t++) if (!strcmp(argv[3], names[t])) { bus_declare(argv[2], t); return 0; }
        err("types: str int bool path pathlist"); return -1;
    }
    char l[LINE], w[LINE];
    if (bus_get(argv[1], NEOS_VIEW_LINUX, l, sizeof l) < 0) { err("no such variable"); return -1; }
    bus_get(argv[1], NEOS_VIEW_WINDOWS, w, sizeof w);
    kprintf("\x1b[36mlinux  \x1b[0m %s\n\x1b[36mwindows\x1b[0m %s\n", l, w);
    return 0;
}

static int c_set(int argc, char **argv, struct ctx *c)       /* cmd.exe style: set NAME=value */
{
    if (argc == 1) return c_vars(argc, argv, c);
    if (c->pure) return -1;
    char *eq = strchr(c->raw, '=');
    if (!eq) { err("usage: set NAME=value   (Windows conventions)"); return -1; }
    char name[40]; size_t nl = (size_t)(eq - c->raw);
    if (nl >= sizeof name) nl = sizeof name - 1;
    memcpy(name, c->raw, nl); name[nl] = 0;
    char *val = eq + 1; size_t vl = strlen(val);
    if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') { val[vl - 1] = 0; val++; }
    return assign(trim(name), val, NEOS_VIEW_WINDOWS);
}

static int c_export(int argc, char **argv, struct ctx *c)    /* POSIX style: export NAME=value */
{
    if (c->pure) return -1;
    if (argc < 2) return c_vars(argc, argv, c);
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { err("usage: export NAME=value   (Linux conventions)"); return -1; }
        *eq = 0;
        if (assign(argv[i], eq + 1, NEOS_VIEW_LINUX)) return -1;
    }
    return 0;
}

static int c_unset(int argc, char **argv, struct ctx *c)
{
    if (c->pure) return -1;
    if (argc < 2) { err("usage: unset <name>"); return -1; }
    if (bus_unset(argv[1], NEOS_VIEW_LINUX)) { err("no such variable"); return -1; }
    return 0;
}

static int c_env(int argc, char **argv, struct ctx *c)
{
    (void)c;
    static char buf[4096];
    int view = view_from(argc > 1 ? argv[1] : NULL);
    bus_env_block(view, buf, sizeof buf);
    kprintf("%s", buf);
    return 0;
}

static int c_path(int argc, char **argv, struct ctx *c)
{
    (void)c;
    char o[LINE];
    if (argc < 3) { err("usage: path <win|lin> <path>    convert a path to that world's convention"); return -1; }
    if (view_from(argv[1]) == NEOS_VIEW_WINDOWS) path_posix_to_win(argv[2], o, sizeof o);
    else path_win_to_posix(argv[2], o, sizeof o);
    kprintf("%s\n", o);
    return 0;
}

static int c_events(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "variable", "ver", "from", "ms_ago", NULL);
    char n[40]; unsigned ver; int view; u64 tk;
    for (int i = 0; !bus_event(i, n, sizeof n, &ver, &view, &tk); i++) {
        if (tb_newrow(c->tb) < 0) break;
        tb_set(c->tb, 0, "%s", n); tb_set(c->tb, 1, "%u", ver);
        tb_set(c->tb, 2, "%s", view == NEOS_VIEW_WINDOWS ? "windows" : view == NEOS_VIEW_LINUX ? "linux" : "neos");
        tb_set(c->tb, 3, "%lu", (ticks - tk) * 1000 / timer_hz());
    }
    return 0;
}

/* ---- program API from the shell (same path a program takes) ---- */
static int c_sys(int argc, char **argv, struct ctx *c)
{
    char buf[LINE]; long r;
    if (c->pure) return -1;
    if (argc >= 3 && !strcmp(argv[1], "tune.get")) r = neos_syscall(SYS_TUNE_GET, (unsigned long)argv[2], (unsigned long)buf, sizeof buf, 0);
    else if (argc >= 4 && !strcmp(argv[1], "tune.set")) { r = neos_syscall(SYS_TUNE_SET, (unsigned long)argv[2], (unsigned long)argv[3], 0, 0); kprintf("rc=%ld\n", r); return 0; }
    else if (argc >= 3 && !strcmp(argv[1], "var.get")) r = neos_syscall(SYS_VAR_GET, (unsigned long)argv[2], (unsigned long)buf, sizeof buf, view_from(argc > 3 ? argv[3] : NULL));
    else if (argc >= 4 && !strcmp(argv[1], "var.set")) { r = neos_syscall(SYS_VAR_SET, (unsigned long)argv[2], (unsigned long)argv[3], view_from(argc > 4 ? argv[4] : NULL), 0); kprintf("rc=%ld (queued to the bus thread)\n", r); return 0; }
    else if (argc >= 2 && !strcmp(argv[1], "time")) { kprintf("%ld ns\n", neos_syscall(SYS_TIME_NS, 0, 0, 0, 0)); return 0; }
    else if (argc >= 2 && !strcmp(argv[1], "modules")) { kprintf("%ld modules\n", neos_syscall(SYS_MODULE_CNT, 0, 0, 0, 0)); return 0; }
    else { err("usage: sys tune.get <n> | tune.set <n> <v> | var.get <n> [lin|win] | var.set <n> <v> [lin|win] | time | modules"); return -1; }
    if (r < 0) { err("syscall failed"); return -1; }
    kprintf("%s\n", buf);
    return 0;
}

static int c_pins(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "slot", "title", "pipeline", NULL);
    char t[24], p[96];
    for (int i = 0; !ui_pins(i, t, sizeof t, p, sizeof p) || i < 4; i++) {
        if (ui_pins(i, t, sizeof t, p, sizeof p)) continue;
        if (tb_newrow(c->tb) < 0) break;
        tb_set(c->tb, 0, "%d", i); tb_set(c->tb, 1, "%s", t); tb_set(c->tb, 2, "%s", p);
    }
    return 0;
}

static int c_unpin(int argc, char **argv, struct ctx *c)
{
    i64 n;
    if (c->pure) return -1;
    if (argc < 2 || parse_i64(argv[1], &n) || ui_unpin((int)n)) { err("usage: unpin <slot>   (see `pins`)"); return -1; }
    return 0;
}

static int c_screen(int argc, char **argv, struct ctx *c) { (void)argc; (void)argv; (void)c; console_dump_screen(); return 0; }

static int c_history(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "n", "command", NULL);
    for (int i = 0; i < nhist && i < 16; i++) { if (tb_newrow(c->tb) < 0) break; tb_set(c->tb, 0, "%d", i + 1); tb_set(c->tb, 1, "%s", hist[(nhist - 1 - i) % 16]); }
    return 0;
}

static int c_reboot(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    if (c->pure) return -1;
    kprintf("rebooting...\n"); console_flush_sync();
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("cli; hlt");
}
static int c_poweroff(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    if (c->pure) return -1;
    kprintf("powering off...\n"); console_flush_sync();
    outw(0x604, 0x2000); outw(0xB004, 0x2000);
    for (;;) __asm__ volatile("cli; hlt");
}

#include "shell_ext.h"      /* network / package / storage commands (added in later stages) */

static const struct cmd cmds[] = {
    { "help",     "help",                      "list commands",                              c_help,   1, 0 },
    { "about",    "about",                     "logo, version and system summary",          c_about,  0, 0 },
    { "clear",    "clear",                     "clear the command stream",                   c_clear,  0, 0 },
    { "echo",     "echo <text>",               "print text (variables are expanded)",        c_echo,   0, 0 },
    { "uptime",   "uptime",                    "time since boot",                            c_uptime, 0, 0 },
    { "ps",       "ps",                        "threads: priority, state, CPU share",        c_ps,     1, 0 },
    { "mods",     "mods",                      "registered kernel modules",                  c_mods,   1, 0 },
    { "tune",     "tune [list|get|set|pending|export]", "view/change module settings (live or at next boot)", c_tune, 1, 1 },
    { "mem",      "mem",                       "memory usage",                               c_mem,    0, 0 },
    { "rt",       "rt [reset]",                "timer jitter / TSC (real-time health)",      c_rt,     0, 0 },
    { "selftest", "selftest",                  "run the real-time scheduler self-test",      c_selftest, 0, 0 },
    { "renice",   "renice <id> <prio>",        "change a thread's priority live (0 = highest)", c_renice, 0, 0 },
    { "kill",     "kill <id>",                 "stop a thread",                              c_kill,   0, 0 },
    { "sleep",    "sleep <ms>",                "wait",                                       c_sleep,  0, 0 },
    { "vars",     "vars",                      "all shared variables in BOTH Linux and Windows form", c_vars, 1, 2 },
    { "var",      "var [name | set n v [lin|win] | type n t]", "inspect/set one variable",  c_var,    1, 2 },
    { "set",      "set NAME=value",            "set a variable, Windows conventions (C:\\x;D:\\y)", c_set, 1, 2 },
    { "export",   "export NAME=value",         "set a variable, Linux conventions (/a:/b)",  c_export, 0, 2 },
    { "unset",    "unset <name>",              "remove a variable",                          c_unset,  0, 2 },
    { "env",      "env [lin|win]",             "environment block as a program of that world would see it", c_env, 0, 0 },
    { "path",     "path <win|lin> <path>",     "translate a path between worlds",            c_path,   0, 0 },
    { "events",   "events",                    "latest variable changes seen by the bus",    c_events, 1, 0 },
    { "sys",      "sys <call> ...",            "call the program API (syscall path)",        c_sys,    0, 0 },
    { "pins",     "pins",                      "pinned live views",                          c_pins,   0, 0 },
    { "unpin",    "unpin <slot>",              "remove a live view from the dock",           c_unpin,  0, 0 },
    { "screen",   "screen",                    "dump the VGA text screen to serial",         c_screen, 0, 0 },
    { "history",  "history",                   "previous commands",                          c_history, 0, 0 },
    SHELL_EXT_CMDS
    { "reboot",   "reboot",                    "restart (applies boot-time settings)",       c_reboot, 0, 0 },
    { "poweroff", "poweroff",                  "shut down",                                  c_poweroff, 0, 0 },
};
#define NCMDS (int)(sizeof cmds / sizeof cmds[0])

static int c_help(int argc, char **argv, struct ctx *c)
{
    (void)argc; (void)argv;
    tb_init(c->tb, "command", "usage", "description", NULL);
    for (int i = 0; i < NCMDS; i++) {
        if (tb_newrow(c->tb) < 0) break;
        tb_set(c->tb, 0, "%s", cmds[i].name); tb_set(c->tb, 1, "%s", cmds[i].usage); tb_set(c->tb, 2, "%s", cmds[i].help);
    }
    return 0;
}


static const struct cmd *find_cmd(const char *name)
{
    for (int i = 0; i < NCMDS; i++) if (!strcmp(cmds[i].name, name)) return &cmds[i];
    return NULL;
}

/* ------------------------------------------------------------ execution */
static int is_assignment(const char *s)
{
    if (!(is_alpha(*s) || *s == '_')) return 0;
    while (is_alpha(*s) || is_digit(*s) || *s == '_' || *s == '.') s++;
    return *s == '=';
}

/* first stage -> table (or direct output), then the filters; *term = index of a terminal pin/json stage, else 0 */
static int eval_stages(char **st, int ns, struct table *tb, int pure, const char **e, int *term)
{
    char line[LINE * 2], full[LINE * 2], *argv[16];
    expand_line(trim(st[0]), line, sizeof line);
    strlcpy(full, line, sizeof full);
    int argc = tokenize(line, argv, 16);
    *term = 0;
    if (!argc) { *e = ""; return -1; }
    const struct cmd *cm = find_cmd(argv[0]);
    if (!cm) { *e = "unknown command (try `help`)"; return -1; }
    if (pure && !cm->pure) { *e = "not a live-safe command"; return -1; }
    char *sp = strchr(full, ' ');
    struct ctx c = { tb, sp ? trim(sp) : "", pure };
    tb->ncols = tb->nrows = tb->more = 0;
    if (cm->fn(argc, argv, &c)) { *e = ""; return -1; }
    for (int i = 1; i < ns; i++) {
        char fl[LINE], *fa[16];
        expand_line(trim(st[i]), fl, sizeof fl);
        int fc = tokenize(fl, fa, 16);
        if (!fc) continue;
        if (!strcmp(fa[0], "pin") || !strcmp(fa[0], "json")) { *term = i; break; }
        if (!tb->ncols) { *e = "command output is not a table"; return -1; }
        if (tb_filter(tb, fc, fa, e)) return -1;
    }
    return 0;
}

int shell_eval_table(const char *pipeline, struct table *out, const char **e)
{
    char buf[LINE], *st[6];
    static const char *dflt = "";
    int term;
    if (!e) e = &dflt;
    strlcpy(buf, pipeline, sizeof buf);
    int ns = split_stages(buf, st, 6);
    if (eval_stages(st, ns, out, 1, e, &term)) return -1;
    if (!out->ncols) { *e = "no table produced"; return -1; }
    return 0;
}

static void run_line(const char *input)
{
    char buf[LINE], *st[6];
    const char *e = "";
    strlcpy(buf, input, sizeof buf);
    char *t = trim(buf);
    if (!*t) return;

    if (is_assignment(t)) {                         /* NAME=value : Linux-style assignment */
        char ex[LINE * 2];
        expand_line(t, ex, sizeof ex);
        char *eq = strchr(ex, '='), *v = eq + 1;
        size_t vl = strlen(v);
        *eq = 0;
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }
        assign(ex, v, NEOS_VIEW_LINUX);
        return;
    }

    int ns = split_stages(t, st, 6), term;
    if (eval_stages(st, ns, &out_tb, 0, &e, &term)) { if (*e) err(e); return; }
    if (term) {
        char fl[LINE], *fa[16];
        expand_line(trim(st[term]), fl, sizeof fl);
        int fc = tokenize(fl, fa, 16);
        if (!out_tb.ncols) { err("command output is not a table"); return; }
        if (!strcmp(fa[0], "json")) { tb_json(&out_tb); return; }
        char pipe[LINE] = "";
        for (int i = 0; i < term; i++) { if (i) strlcat(pipe, " | ", sizeof pipe); strlcat(pipe, trim(st[i]), sizeof pipe); }
        char title[24];
        if (fc > 1) strlcpy(title, fa[1], sizeof title);
        else { char *sp = strchr(pipe, ' '); size_t n = sp ? (size_t)(sp - pipe) : strlen(pipe); if (n >= sizeof title) n = sizeof title - 1; memcpy(title, pipe, n); title[n] = 0; }
        int slot = ui_pin(title, pipe, &e);
        if (slot < 0) err(e); else kprintf("pinned to dock slot %d (\x1b[90m%s\x1b[0m)\n", slot, pipe);
        return;
    }
    if (out_tb.ncols) tb_render(&out_tb);
}

/* ------------------------------------------------------------ line editor */
static void redraw(const char *buf, int len, int pos)
{
    kprintf("\r%s%s\x1b[K", PROMPT, buf);
    if (len > pos) kprintf("\x1b[%dD", len - pos);
}

#define MAXC 40
static int gather(const char *prefix, int kind, int first, const char **out)
{
    int n = 0, pl = (int)strlen(prefix);
    if (first) {
        for (int i = 0; i < NCMDS && n < MAXC; i++) if (!strncmp(cmds[i].name, prefix, pl)) out[n++] = cmds[i].name;
    } else if (kind == 1) {
        for (int i = 0; i < tune_count() && n < MAXC; i++) if (!strncmp(tune_at(i)->name, prefix, pl)) out[n++] = tune_at(i)->name;
    } else if (kind == 2) {
        static char names[MAXC][40];
        for (int i = 0; i < bus_count() && n < MAXC; i++) {
            struct bus_var_info vi;
            if (!bus_info(i, &vi) && !strncmp(vi.name, prefix, pl)) { strlcpy(names[n], vi.name, 40); out[n] = names[n]; n++; }
        }
    }
    return n;
}

static void complete(char *buf, int *len, int *pos)
{
    int ts = *pos;
    while (ts > 0 && buf[ts - 1] != ' ') ts--;
    char prefix[48];
    int pl = *pos - ts;
    if (pl >= (int)sizeof prefix) return;
    memcpy(prefix, buf + ts, pl); prefix[pl] = 0;
    int first = 1;
    for (int i = 0; i < ts; i++) if (buf[i] != ' ') first = 0;
    int kind = 0;
    if (!first) {
        char cmdn[24]; int k = 0, i = 0;
        while (buf[i] == ' ') i++;
        while (buf[i] && buf[i] != ' ' && k < 23) cmdn[k++] = buf[i++];
        cmdn[k] = 0;
        const struct cmd *cm = find_cmd(cmdn);
        if (cm) kind = cm->complete;
    }
    const char *m[MAXC];
    int n = gather(prefix, kind, first, m);
    if (!n) return;
    int common = (int)strlen(m[0]);
    for (int i = 1; i < n; i++) { int j = 0; while (j < common && m[i][j] == m[0][j]) j++; common = j; }
    if (n > 1 && common == pl) {
        kprintf("\n");
        for (int i = 0; i < n; i++) kprintf("%s  ", m[i]);
        kprintf("\n");
        redraw(buf, *len, *pos);
        return;
    }
    int add = common - pl + (n == 1 ? 1 : 0);
    if (*len + add >= LINE - 1) return;
    memmove(buf + *pos + add, buf + *pos, *len - *pos + 1);
    memcpy(buf + *pos, m[0] + pl, common - pl);
    if (n == 1) buf[*pos + common - pl] = ' ';
    *len += add; *pos += add;
    redraw(buf, *len, *pos);
}

static int readline(char *buf)
{
    int len = 0, pos = 0, hi = -1;
    buf[0] = 0;
    kprintf("%s", PROMPT);
    for (;;) {
        int k = key_poll();
        if (k < 0) { thread_sleep_ms(2); continue; }
        if (k == '\n') { kprintf("\n"); return len; }
        if (k == 3) { kprintf("^C\n"); buf[0] = 0; return 0; }
        if (k == 12) { console_clear_stream(); redraw(buf, len, pos); continue; }
        if (k == '\t') { complete(buf, &len, &pos); continue; }
        if (k == '\b' || k == 8) {
            if (!pos) continue;
            memmove(buf + pos - 1, buf + pos, len - pos + 1);
            pos--; len--;
            if (pos == len) kprintf("\b \b"); else redraw(buf, len, pos);
        } else if (k == K_DEL) {
            if (pos == len) continue;
            memmove(buf + pos, buf + pos + 1, len - pos);
            len--; redraw(buf, len, pos);
        } else if (k == K_LEFT)  { if (pos) { pos--; kprintf("\x1b[D"); } }
        else if (k == K_RIGHT)   { if (pos < len) { pos++; kprintf("\x1b[C"); } }
        else if (k == K_HOME || k == 1) { pos = 0; redraw(buf, len, pos); }
        else if (k == K_END || k == 5)  { pos = len; redraw(buf, len, pos); }
        else if (k == 21) { len = pos = 0; buf[0] = 0; redraw(buf, len, pos); }
        else if (k == K_UP || k == K_DOWN) {
            int total = nhist < 16 ? nhist : 16;
            if (k == K_UP && hi + 1 < total) hi++;
            else if (k == K_DOWN && hi >= 0) hi--;
            else continue;
            if (hi < 0) buf[0] = 0; else strlcpy(buf, hist[(nhist - 1 - hi) % 16], LINE);
            len = pos = (int)strlen(buf);
            redraw(buf, len, pos);
        } else if (k >= 32 && k < 127 && len < LINE - 2) {
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos++] = (char)k; len++;
            if (pos == len) kputc((char)k); else redraw(buf, len, pos);
        }
    }
}

static void shell_thread(void *arg)
{
    (void)arg;
    char line[LINE];
    kprintf("Type \x1b[1mhelp\x1b[0m for commands, \x1b[1mTab\x1b[0m completes, "
            "\x1b[1m<cmd> | pin\x1b[0m keeps a live view on screen.\n\n");
    for (;;) {
        int n = readline(line);
        if (n && (!nhist || strcmp(hist[(nhist - 1) % 16], line))) strlcpy(hist[nhist++ % 16], line, LINE);
        run_line(line);
    }
}

void shell_start(void)
{
    module_register("shell", "1.0", "unified shell: tables, pipelines, pins, Linux+Windows variable syntax");
    t_prio = tune_int("shell", "prio", "shell thread priority", TF_BOOT, 4, 0, 6, NULL);
    thread_create("shell", (u8)t_prio->val, shell_thread, NULL);
}
