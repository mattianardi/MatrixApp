#include "agent.h"
#include "fs.h"
#include "net.h"
#include "shell.h"
#include "varbus.h"

/* Everything an agent can do. Each tool names the capability it needs and its risk class; `check`
 * validates scopes BEFORE the operator is ever asked, `run` produces a compact JSON answer. */

static int fail(struct jbuf *o, int code, const char *m) { o->len = 0; o->b[0] = 0; jp(o, "%s", m); return code; }
#define NEEDARGS(n, usage) do { if (argc < (n)) { *why = "usage: " usage; return 400; } } while (0)
#define NEEDARGS_RUN(n, usage) do { if (argc < (n)) return fail(o, 400, "usage: " usage); } while (0)

static int num(const char *s, i64 *v) { return parse_i64(s, v); }

static const char *view_name(int v) { return v == NEOS_VIEW_WINDOWS ? "windows" : v == NEOS_VIEW_LINUX ? "linux" : "neos"; }
static int view_arg(const char *s) { return s && (!strcmp(s, "win") || !strcmp(s, "windows")) ? NEOS_VIEW_WINDOWS : NEOS_VIEW_LINUX; }

/* ------------------------------------------------------------ observe */
static int t_whoami(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc; (void)argv;
    jp(o, "{\"agent\":\"%s\",\"kind\":\"%s\",\"caps\":{", a->name, a->kind == AK_NATIVE ? "native" : "remote");
    int first = 1;
    for (int i = 0; i < NCAPS; i++) if (a->caps & (1u << i)) { jp(o, "%s\"%s\":", first ? "" : ",", cap_names[i]); jstr(o, a->scope[i][0] ? a->scope[i] : "(no scope)"); first = 0; }
    static const char *pn[] = { "allow", "ask", "deny" };
    jp(o, "},\"policy\":{\"read\":\"%s\",\"reversible\":\"%s\",\"irreversible\":\"%s\"},\"frozen\":%s}",
       pn[a->policy[0]], pn[a->policy[1]], pn[a->policy[2]], agent_is_frozen() ? "true" : "false");
    return 0;
}

static int t_tools(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc; (void)argv;
    static const char *rn[] = { "read", "reversible", "irreversible" };
    jp(o, "[");
    int first = 1;
    for (int i = 0; i < agent_ntools; i++) {
        const struct tool *t = &agent_tools[i];
        if (!(a->caps & (1u << t->cap))) continue;                     /* only what this agent may use */
        jp(o, "%s{\"name\":\"%s\",\"params\":\"%s\",\"cap\":\"%s\",\"risk\":\"%s\",\"desc\":", first ? "" : ",", t->name, t->params, cap_names[t->cap], rn[t->risk]);
        jstr(o, t->desc); jp(o, "}");
        first = 0;
    }
    jp(o, "]");
    return 0;
}

static int t_sysinfo(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    int th = 0; struct thread_info ti;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) th += !thread_info(i, &ti);
    jp(o, "{\"os\":\"%s\",\"version\":\"%s\",\"uptime_ms\":%lu,\"tick_hz\":%u,\"cpu\":", NEOS_NAME, NEOS_VERSION, time_ns() / 1000000, timer_hz());
    jstr(o, cpu_brand());
    jp(o, ",\"mem_free_kib\":%lu,\"mem_total_kib\":%lu,\"threads\":%d,\"modules\":%d,\"tunables\":%d,\"net_up\":%s}",
       (u64)pmm_free_pages() * 4, (u64)pmm_total_pages() * 4, th, module_count(), tune_count(), net_up() ? "true" : "false");
    return 0;
}

static int t_modlist(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    jp(o, "[");
    for (int i = 0; i < module_count(); i++) {
        const char *n, *v, *d; module_get(i, &n, &v, &d);
        int cnt = 0; for (int k = 0; k < tune_count(); k++) cnt += !strcmp(tune_at(k)->module, n);
        jp(o, "%s{\"name\":\"%s\",\"version\":\"%s\",\"tunables\":%d,\"desc\":", i ? "," : "", n, v, cnt); jstr(o, d); jp(o, "}");
    }
    jp(o, "]");
    return 0;
}

static int t_threads(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    u64 total = sched_runticks_total() ? sched_runticks_total() : 1; int first = 1;
    jp(o, "[");
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        struct thread_info ti;
        if (thread_info(i, &ti)) continue;
        jp(o, "%s{\"id\":%d,\"name\":\"%s\",\"prio\":%d,\"state\":\"%s\",\"cpu_permille\":%lu,\"stack_kib\":%d,\"wake_latency_max_us\":%lu,\"protected\":%s}",
           first ? "" : ",", ti.id, ti.name, ti.prio, ti.state, ti.runticks * 1000 / total, ti.stack_kib, ti.lat_max_ns / 1000, thread_protected(i) ? "true" : "false");
        first = 0;
    }
    jp(o, "]");
    return 0;
}

static int t_meminfo(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    size_t u, p, t; heap_stats(&u, &p, &t);
    jp(o, "{\"phys_free_kib\":%lu,\"phys_total_kib\":%lu,\"heap_used\":%lu,\"heap_peak\":%lu,\"heap_size\":%lu,\"fs_files\":%d,\"fs_bytes\":%lu,\"console_dropped\":%lu}",
       (u64)pmm_free_pages() * 4, (u64)pmm_total_pages() * 4, (u64)u, (u64)p, (u64)t, fs_count(), (u64)fs_bytes(), console_dropped());
    return 0;
}

static int t_rtstats(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    u64 mn, mx, avg, n, lm, la, ln, ia, im, in;
    timer_jitter(&mn, &mx, &avg, &n); sched_latency(&lm, &la, &ln); timer_isr_stats(&ia, &im, &in);
    jp(o, "{\"tick_hz\":%u,\"tsc_mhz\":%lu,\"invariant_tsc\":%s,\"tick_period_ns\":{\"min\":%lu,\"avg\":%lu,\"max\":%lu,\"samples\":%lu},"
          "\"rt_wake_latency_ns\":{\"max\":%lu,\"avg\":%lu,\"samples\":%lu},\"tick_isr_ns\":{\"avg\":%lu,\"max\":%lu}}",
       timer_hz(), tsc_hz() / 1000000, cpu_invariant_tsc() ? "true" : "false", mn, avg, mx, n, lm, la, ln, cycles_to_ns(ia), cycles_to_ns(im));
    return 0;
}

static int t_tunelist(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a;
    const char *pfx = argc > 1 ? argv[1] : ""; i64 off = 0;
    if (argc > 2) num(argv[2], &off);
    int shown = 0, seen = 0;
    jp(o, "{\"items\":[");
    for (int i = 0; i < tune_count(); i++) {
        struct tunable *t = tune_at(i);
        if (strncmp(t->name, pfx, strlen(pfx))) continue;
        if (seen++ < off) continue;
        if (shown == 12) { jp(o, "],\"next\":%ld}", (i64)seen - 1); return 0; }
        char v[64]; tune_format(t, v, sizeof v);
        jp(o, "%s{\"name\":\"%s\",\"value\":", shown ? "," : "", t->name); jstr(o, v);
        jp(o, ",\"type\":\"%s\",\"mode\":\"%s\",\"agent_writable\":%s", t->type == TT_INT ? "int" : t->type == TT_BOOL ? "bool" : "str",
           t->flags & TF_RO ? "ro" : t->flags & TF_BOOT ? "boot" : "live", (t->flags & (TF_RO | TF_NOAGENT)) ? "false" : "true");
        if (t->type == TT_INT) jp(o, ",\"min\":%ld,\"max\":%ld,\"default\":%ld", t->min, t->max, t->def);
        jp(o, ",\"desc\":"); jstr(o, t->desc); jp(o, "}");
        shown++;
    }
    jp(o, "],\"next\":null}");
    return 0;
}

static int t_tuneget(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a;
    NEEDARGS_RUN(2, "tune.get <name>");
    struct tunable *t = tune_find(argv[1]);
    if (!t) return fail(o, 404, "no such setting");
    char v[64]; tune_format(t, v, sizeof v);
    jp(o, "{\"name\":\"%s\",\"value\":", t->name); jstr(o, v); jp(o, "}");
    return 0;
}

static int t_varlist(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    jp(o, "[");
    for (int i = 0; i < bus_count(); i++) {
        struct bus_var_info vi; char l[160], w[160];
        if (bus_info(i, &vi)) continue;
        bus_get(vi.name, NEOS_VIEW_LINUX, l, sizeof l); bus_get(vi.name, NEOS_VIEW_WINDOWS, w, sizeof w);
        jp(o, "%s{\"name\":\"%s\",\"type\":\"%s\",\"linux\":", i ? "," : "", vi.name, bus_type_name(vi.type)); jstr(o, l);
        jp(o, ",\"windows\":"); jstr(o, w); jp(o, ",\"set_by\":\"%s\",\"ver\":%u}", view_name(vi.view), vi.ver);
    }
    jp(o, "]");
    return 0;
}

static int t_varget(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a;
    NEEDARGS_RUN(2, "var.get <name> [lin|win]");
    char v[256];
    if (bus_get(argv[1], view_arg(argc > 2 ? argv[2] : NULL), v, sizeof v) < 0) return fail(o, 404, "no such variable");
    jp(o, "{\"name\":\"%s\",\"value\":", argv[1]); jstr(o, v); jp(o, "}");
    return 0;
}

static int t_audit(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a;
    i64 n = 10; if (argc > 1) num(argv[1], &n);
    if (n > 20) n = 20;
    jp(o, "[");
    struct audit_rec r;
    for (int i = 0; i < n && !audit_get(i, &r); i++) {
        struct agent *ag = agent_at(r.agent);
        jp(o, "%s{\"seq\":%lu,\"agent\":\"%s\",\"tool\":\"%s\",\"args\":", i ? "," : "", r.seq, ag ? ag->name : "?", r.tool); jstr(o, r.args);
        jp(o, ",\"rc\":%d,\"approver\":\"%s\",\"hash\":\"%.12s\"}", r.rc, r.approver ? "operator" : "-", r.hash);
    }
    jp(o, "]");
    return 0;
}

static int t_netinfo(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    static const char *k[] = { "mac", "ip", "mask", "gateway", "dns" };
    jp(o, "{");
    for (int i = 0; i < 5; i++) { char key[24]; ksnprintf(key, sizeof key, "net.%s", k[i]); struct tunable *t = tune_find(key); jp(o, "%s\"%s\":", i ? "," : "", k[i]); jstr(o, t ? t->sval : ""); }
    jp(o, ",\"up\":%s}", net_up() ? "true" : "false");
    return 0;
}

/* ------------------------------------------------------------ tuning */
static int tune_check_one(struct agent *a, const char *name, const char *val, const char **why)
{
    struct tunable *t = tune_find(name);
    if (!t) { *why = "no such setting"; return 404; }
    if (t->flags & (TF_RO | TF_NOAGENT)) { *why = "setting is protected: only the operator may change it"; return 403; }
    i64 v = 0; int has = t->type != TT_STR && !num(val, &v);
    if (t->type == TT_BOOL) { has = 1; v = (!strcasecmp(val, "on") || !strcmp(val, "1") || !strcasecmp(val, "true")); }
    if (!scope_allows(a, CAP_TUNE, name, has, v)) { *why = "outside this agent's tune scope (name or value range)"; return 403; }
    return 0;
}

static int c_tuneset(struct agent *a, int argc, char **argv, const char **why)
{
    NEEDARGS(3, "tune.set <name> <value>");
    return tune_check_one(a, argv[1], argv[2], why);
}

static int t_tuneset(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc;
    struct tunable *t = tune_find(argv[1]); char old[64], cur[64]; const char *m = "";
    tune_format(t, old, sizeof old);
    int r = tune_set(argv[1], argv[2], &m);
    if (r < 0) return fail(o, 400, m);
    journal_push(a, J_TUNE, 0, t->name, old, NULL, 0, 1);
    tune_format(t, cur, sizeof cur);
    jp(o, "{\"name\":\"%s\",\"old\":", t->name); jstr(o, old); jp(o, ",\"new\":"); jstr(o, argv[2]); jp(o, ",\"applied\":%s,\"note\":", r == 0 ? "true" : "false"); jstr(o, m); jp(o, "}");
    return 0;
}

static int c_varset(struct agent *a, int argc, char **argv, const char **why)
{
    NEEDARGS(3, "var.set <name> <value> [lin|win]");
    if (!strncmp(argv[1], "NEOS.", 5)) {                       /* the NEOS.* namespace IS the tunables */
        if (!(a->caps & (1u << CAP_TUNE))) { *why = "NEOS.* variables need the tune capability"; return 403; }
        return tune_check_one(a, argv[1] + 5, argv[2], why);
    }
    if (!scope_allows(a, CAP_VARS, argv[1], 0, 0)) { *why = "outside this agent's vars scope"; return 403; }
    return 0;
}

static int t_varset(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    char old[256] = ""; const char *m = "";
    int view = view_arg(argc > 3 ? argv[3] : NULL);
    int had = bus_get(argv[1], NEOS_VIEW_LINUX, old, sizeof old) >= 0;
    if (bus_set(argv[1], argv[2], view, 1, &m)) return fail(o, 400, m);
    if (strncmp(argv[1], "NEOS.", 5)) journal_push(a, J_VAR, 0, argv[1], old, NULL, 0, had);
    char l[256], w[256];
    bus_get(argv[1], NEOS_VIEW_LINUX, l, sizeof l); bus_get(argv[1], NEOS_VIEW_WINDOWS, w, sizeof w);
    jp(o, "{\"name\":\"%s\",\"linux\":", argv[1]); jstr(o, l); jp(o, ",\"windows\":"); jstr(o, w); jp(o, "}");
    return 0;
}

static int c_varunset(struct agent *a, int argc, char **argv, const char **why)
{
    NEEDARGS(2, "var.unset <name>");
    if (!scope_allows(a, CAP_VARS, argv[1], 0, 0)) { *why = "outside this agent's vars scope"; return 403; }
    return 0;
}
static int t_varunset(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc;
    char old[256] = "";
    if (bus_get(argv[1], NEOS_VIEW_LINUX, old, sizeof old) < 0) return fail(o, 404, "no such variable");
    bus_unset(argv[1], NEOS_VIEW_LINUX);
    journal_push(a, J_VAR, 0, argv[1], old, NULL, 0, 1);
    jp(o, "{\"removed\":\"%s\"}", argv[1]);
    return 0;
}

/* ------------------------------------------------------------ threads */
static int c_renice(struct agent *a, int argc, char **argv, const char **why)
{
    i64 id, pr;
    NEEDARGS(3, "thread.renice <id> <prio>");
    if (num(argv[1], &id) || num(argv[2], &pr)) { *why = "numbers expected"; return 400; }
    struct thread_info ti;
    if (thread_info((int)id, &ti)) { *why = "no such thread"; return 404; }
    if (thread_protected((int)id)) { *why = "core system thread: protected"; return 403; }
    int mp = scope_minprio(a);
    if (ti.prio < mp || pr < mp || pr > SCHED_PRIO_IDLE) { *why = "priority outside the agent's allowed band (real-time threads are off limits)"; return 403; }
    return 0;
}
static int t_renice(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc;
    i64 id, pr; struct thread_info ti; char old[8];
    num(argv[1], &id); num(argv[2], &pr); thread_info((int)id, &ti);
    ksnprintf(old, sizeof old, "%d", ti.prio);
    if (thread_set_prio((int)id, (int)pr)) return fail(o, 400, "cannot change priority");
    journal_push(a, J_PRIO, (int)id, ti.name, old, NULL, 0, 1);
    jp(o, "{\"thread\":\"%s\",\"old\":%s,\"new\":%ld}", ti.name, old, pr);
    return 0;
}

static int c_kill(struct agent *a, int argc, char **argv, const char **why)
{
    i64 id;
    NEEDARGS(2, "thread.kill <id>");
    if (num(argv[1], &id)) { *why = "number expected"; return 400; }
    struct thread_info ti;
    if (thread_info((int)id, &ti)) { *why = "no such thread"; return 404; }
    if (thread_protected((int)id) || ti.prio < scope_minprio(a)) { *why = "thread is protected or outside the agent's priority band"; return 403; }
    return 0;
}
static int t_kill(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc;
    i64 id; num(argv[1], &id);
    if (thread_kill((int)id)) return fail(o, 400, "cannot kill");
    jp(o, "{\"killed\":%ld}", id);
    return 0;
}

/* ------------------------------------------------------------ filesystem */
static int fs_path(const char *in, char *out, size_t n, const char **why)
{
    if (in[0] != '/') { *why = "absolute paths only"; return 400; }
    fs_norm(in, out, n);
    return 0;
}

static int c_fsls(struct agent *a, int argc, char **argv, const char **why)
{
    char p[128], d[130]; int r;
    NEEDARGS(2, "fs.ls <dir>");
    if ((r = fs_path(argv[1], p, sizeof p, why))) return r;
    ksnprintf(d, sizeof d, "%s/", p);
    if (!scope_allows(a, CAP_FSR, d, 0, 0) && !scope_allows(a, CAP_FSW, d, 0, 0)) { *why = "outside this agent's fs scope"; return 403; }
    return 0;
}
static int t_fsls(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc;
    char dir[128], p[128]; size_t sz;
    fs_norm(argv[1], dir, sizeof dir);
    size_t dl = strlen(dir); if (dl == 1) dl = 0;
    char seen[24][40]; int ns = 0;
    jp(o, "[");
    for (int i = 0; !fs_info(i, p, sizeof p, &sz); i++) {
        if (strncmp(p, dir, dl) || p[dl] != '/') continue;
        char *rest = p + dl + 1, *sl = strchr(rest, '/');
        char name[40]; size_t nl = sl ? (size_t)(sl - rest) : strlen(rest); if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, rest, nl); name[nl] = 0;
        int dup = 0; for (int k = 0; k < ns; k++) dup |= !strcmp(seen[k], name);
        if (dup || ns == 24) continue;
        strlcpy(seen[ns++], name, 40);
        jp(o, "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lu}", ns > 1 ? "," : "", name, sl ? "dir" : "file", sl ? 0ul : (u64)sz);
    }
    jp(o, "]");
    return 0;
}

static int c_fsread(struct agent *a, int argc, char **argv, const char **why)
{
    char p[128]; int r;
    NEEDARGS(2, "fs.read <path> [offset] [len]");
    if ((r = fs_path(argv[1], p, sizeof p, why))) return r;
    if (!scope_allows(a, CAP_FSR, p, 0, 0) && !scope_allows(a, CAP_FSW, p, 0, 0)) { *why = "outside this agent's fs scope"; return 403; }
    return 0;
}
static int t_fsread(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a;
    char p[128]; const u8 *d; size_t n; i64 off = 0, len = 512;
    fs_norm(argv[1], p, sizeof p);
    if (fs_read(p, &d, &n)) return fail(o, 404, "no such file");
    if (argc > 2) num(argv[2], &off);
    if (argc > 3) num(argv[3], &len);
    if (len > 768) len = 768;
    if (off < 0 || (size_t)off > n) off = (i64)n;
    if ((size_t)(off + len) > n) len = (i64)n - off;
    char enc[1100];
    b64_encode(d + off, (size_t)len, enc, sizeof enc);
    jp(o, "{\"size\":%lu,\"offset\":%ld,\"len\":%ld,\"data_b64\":\"%s\"}", (u64)n, off, len, enc);
    return 0;
}

static int c_fswrite(struct agent *a, int argc, char **argv, const char **why)
{
    char p[128]; int r;
    NEEDARGS(3, "fs.write <path> <base64> [append]");
    if ((r = fs_path(argv[1], p, sizeof p, why))) return r;
    if (!scope_allows(a, CAP_FSW, p, 0, 0)) { *why = "outside this agent's fs.write scope"; return 403; }
    return 0;
}
static int t_fswrite(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    char p[128]; const u8 *old; size_t oldn = 0; int had;
    fs_norm(argv[1], p, sizeof p);
    u8 *chunk = kmalloc(2048);
    if (!chunk) return fail(o, 500, "out of memory");
    int cn = b64_decode(argv[2], chunk, 2048);
    if (cn < 0) { kfree(chunk); return fail(o, 400, "bad base64 (max 2 KiB per call; use append for more)"); }
    int append = argc > 3 && !strcmp(argv[3], "append");
    had = fs_read(p, &old, &oldn) == 0;
    size_t newn = (append && had ? oldn : 0) + (size_t)cn;
    if (a->fs_used + newn > (size_t)tune_find("agent.fs_quota_kib")->val * 1024 + oldn) { kfree(chunk); return fail(o, 507, "agent filesystem quota exceeded"); }
    u8 *buf = kmalloc(newn ? newn : 1);
    if (!buf) { kfree(chunk); return fail(o, 500, "out of memory"); }
    size_t k = 0;
    if (append && had) { memcpy(buf, old, oldn); k = oldn; }
    memcpy(buf + k, chunk, cn);
    if (had && oldn <= 65536) journal_push(a, J_FS, 0, p, NULL, old, oldn, 1); else if (!had) journal_push(a, J_FS, 0, p, NULL, NULL, 0, 0);
    int r = fs_write(p, buf, newn);
    kfree(buf); kfree(chunk);
    if (r) return fail(o, 500, "filesystem full");
    a->fs_used = a->fs_used - (had ? oldn : 0) + newn;
    jp(o, "{\"path\":\"%s\",\"size\":%lu}", p, (u64)newn);
    return 0;
}

static int t_fsrm(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc;
    char p[128]; const u8 *d; size_t n;
    fs_norm(argv[1], p, sizeof p);
    if (fs_read(p, &d, &n)) return fail(o, 404, "no such file");
    if (n <= 65536) journal_push(a, J_FS, 0, p, NULL, d, n, 1);
    fs_remove(p);
    a->fs_used = a->fs_used > n ? a->fs_used - n : 0;
    jp(o, "{\"removed\":\"%s\"}", p);
    return 0;
}

/* ------------------------------------------------------------ packages */
static int c_pkgname(struct agent *a, int argc, char **argv, const char **why)
{
    NEEDARGS(2, "pkg.<install|remove> <name>");
    if (!scope_allows(a, CAP_PKG, argv[1], 0, 0)) { *why = "package outside this agent's pkg scope"; return 403; }
    return 0;
}
static int t_pkgupdate(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    const char *e = "";
    if (pkg_update(&e)) return fail(o, 502, e);
    jp(o, "{\"ok\":true}");
    return 0;
}
static int t_pkglist(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc; (void)argv;
    static struct table tb;
    pkg_table(&tb);
    jp(o, "[");
    for (int i = 0; i < tb.nrows; i++) {
        jp(o, "%s{\"name\":\"%s\",\"version\":\"%s\",\"size\":%s,\"installed\":%s}", i ? "," : "", tb.cell[i][0], tb.cell[i][1], tb.cell[i][2], strcmp(tb.cell[i][3], "installed") ? "false" : "true");
    }
    jp(o, "]");
    return 0;
}
static int t_pkginstall(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc;
    const char *e = ""; char rec[64]; const u8 *d; size_t n;
    ksnprintf(rec, sizeof rec, "/var/pkg/%s", argv[1]);
    int had = fs_read(rec, &d, &n) == 0;
    if (pkg_install(argv[1], &e)) return fail(o, 502, e);
    if (!had) journal_push(a, J_PKG, 0, argv[1], NULL, NULL, 0, 0);
    jp(o, "{\"installed\":\"%s\"}", argv[1]);
    return 0;
}
static int t_pkgremove(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc;
    const char *e = "";
    if (pkg_remove(argv[1], &e)) return fail(o, 404, e);
    jp(o, "{\"removed\":\"%s\"}", argv[1]);
    return 0;
}
static int c_pkgbuild(struct agent *a, int argc, char **argv, const char **why)
{
    char d[128]; int r;
    NEEDARGS(4, "pkg.build <name> <version> <dir>");
    if (!scope_allows(a, CAP_PKG, argv[1], 0, 0)) { *why = "package name outside this agent's pkg scope"; return 403; }
    if ((r = fs_path(argv[3], d, sizeof d, why))) return r;
    ksnprintf(d + strlen(d), sizeof d - strlen(d), "/");
    if (!scope_allows(a, CAP_FSR, d, 0, 0) && !scope_allows(a, CAP_FSW, d, 0, 0)) { *why = "source directory outside this agent's fs scope"; return 403; }
    return 0;
}
static int t_pkgbuild(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc;
    char dir[128], out[96], sha[65]; size_t sz; const char *e = "";
    fs_norm(argv[3], dir, sizeof dir);
    int n = pkg_build(argv[1], argv[2], dir, out, sizeof out, sha, &sz, &e);
    if (n < 0) return fail(o, 400, e);
    jp(o, "{\"package\":\"%s\",\"files\":%d,\"size\":%lu,\"sha256\":\"%s\"}", out, n, (u64)sz, sha);
    return 0;
}
static int t_pkginstfile(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)argc;
    char p[128]; const u8 *d; size_t n; const char *e = "";
    fs_norm(argv[3], p, sizeof p);
    if (fs_read(p, &d, &n)) return fail(o, 404, "no such file");
    char rec[64]; const u8 *x; size_t xn;
    ksnprintf(rec, sizeof rec, "/var/pkg/%s", argv[1]);
    int had = fs_read(rec, &x, &xn) == 0;
    int files = pkg_install_blob(argv[1], argv[2], d, n, &e);
    if (files < 0) return fail(o, 400, e);
    if (!had) journal_push(a, J_PKG, 0, argv[1], NULL, NULL, 0, 0);
    jp(o, "{\"installed\":\"%s\",\"version\":\"%s\",\"files\":%d,\"root\":\"/opt/%s\"}", argv[1], argv[2], files, argv[1]);
    return 0;
}
static int c_pkginstfile(struct agent *a, int argc, char **argv, const char **why)
{
    char p[128]; int r;
    NEEDARGS(4, "pkg.install_file <name> <version> <npk-path>");
    if (!scope_allows(a, CAP_PKG, argv[1], 0, 0)) { *why = "package name outside this agent's pkg scope"; return 403; }
    if ((r = fs_path(argv[3], p, sizeof p, why))) return r;
    if (!scope_allows(a, CAP_FSR, p, 0, 0) && !scope_allows(a, CAP_FSW, p, 0, 0)) { *why = "file outside this agent's fs scope"; return 403; }
    return 0;
}

/* ------------------------------------------------------------ network */
static int c_netget(struct agent *a, int argc, char **argv, const char **why)
{
    char host[64] = "", p[128]; int r;
    NEEDARGS(3, "net.get <http-url> <dest-path>");
    if (strncmp(argv[1], "http://", 7)) { *why = "http:// URLs only (no TLS yet)"; return 400; }
    size_t k = 0; for (const char *s = argv[1] + 7; *s && *s != '/' && *s != ':' && k < sizeof host - 1; s++) host[k++] = *s;
    host[k] = 0;
    if (!scope_allows(a, CAP_NET, host, 0, 0)) { *why = "host outside this agent's net scope"; return 403; }
    if ((r = fs_path(argv[2], p, sizeof p, why))) return r;
    if (!scope_allows(a, CAP_FSW, p, 0, 0)) { *why = "destination outside this agent's fs.write scope"; return 403; }
    return 0;
}
static int t_netget(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc;
    size_t cap = 256 * 1024, n = 0; int st = 0; const char *e = ""; char p[128];
    u8 *buf = kmalloc(cap);
    if (!buf) return fail(o, 500, "out of memory");
    if (http_get(argv[1], buf, cap, &n, &st, &e)) { kfree(buf); return fail(o, 502, e); }
    fs_norm(argv[2], p, sizeof p);
    int r = fs_write(p, buf, n);
    kfree(buf);
    if (r) return fail(o, 500, "filesystem full");
    jp(o, "{\"status\":%d,\"bytes\":%lu,\"saved\":\"%s\"}", st, (u64)n, p);
    return 0;
}

/* ------------------------------------------------------------ debug (read only) */
static int c_peek(struct agent *a, int argc, char **argv, const char **why)
{
    i64 addr, len;
    (void)a;
    NEEDARGS(3, "debug.peek <addr> <len<=128>");
    if (num(argv[1], &addr) || num(argv[2], &len) || len < 1 || len > 128) { *why = "bad address/length"; return 400; }
    if (addr < 0x1000 || (u64)addr + len > (u64)pmm_total_pages() * 4096 + 0x200000) { *why = "address outside RAM"; return 403; }
    return 0;
}
static int t_peek(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    (void)a; (void)argc;
    i64 addr, len; num(argv[1], &addr); num(argv[2], &len);
    char enc[200];
    b64_encode((const void *)addr, (size_t)len, enc, sizeof enc);
    jp(o, "{\"addr\":\"0x%lx\",\"len\":%ld,\"data_b64\":\"%s\"}", addr, len, enc);
    return 0;
}

/* ------------------------------------------------------------ metrics + experiments */
static struct tunable *t_weight;
static u64 busy_idle0, busy_total0;

void metric_reset(void)
{
    timer_jitter_reset(); sched_latency_reset();
    struct thread_info ti;
    busy_idle0 = !thread_info(0, &ti) ? ti.runticks : 0;
    busy_total0 = sched_runticks_total();
}

int metric_value(const char *name, i64 *out)
{
    u64 lm, la, ln, ia, im, in, mn, mx, avg, n;
    sched_latency(&lm, &la, &ln); timer_isr_stats(&ia, &im, &in); timer_jitter(&mn, &mx, &avg, &n);
    if (!t_weight) t_weight = tune_int("agent", "cost_weight", "rt.cost: microseconds charged per 1% of tick-handler CPU overhead", TF_LIVE | TF_NOAGENT, 200, 0, 100000, NULL);
    i64 bound_us = (i64)(1000000 / timer_hz()) + (i64)(lm / 1000);
    i64 overhead_x100 = (i64)(cycles_to_ns(ia) * timer_hz() / 1000 / 100);       /* percent * 100 */
    if (!strcmp(name, "rt.bound_us")) *out = bound_us;
    else if (!strcmp(name, "rt.overhead_x100")) *out = overhead_x100;
    else if (!strcmp(name, "rt.cost")) *out = bound_us + t_weight->val * overhead_x100 / 100;
    else if (!strcmp(name, "timer.jitter_us")) *out = (i64)((mx - mn) / 1000);
    else if (!strcmp(name, "cpu.busy_pct")) {
        struct thread_info ti; u64 idle = !thread_info(0, &ti) ? ti.runticks : 0, tot = sched_runticks_total();
        *out = tot > busy_total0 ? (i64)(100 - (idle - busy_idle0) * 100 / (tot - busy_total0)) : 0;
    } else return -1;
    return 0;
}

#define MAXCHG 4
static int c_exp(struct agent *a, int argc, char **argv, const char **why)
{
    i64 w, mv;
    NEEDARGS(4, "experiment.run <metric> <window_ms> <name=value>...");
    if (metric_value(argv[1], &mv)) { *why = "unknown metric (rt.cost rt.bound_us rt.overhead_x100 timer.jitter_us cpu.busy_pct)"; return 400; }
    if (num(argv[2], &w) || w < 50 || w > 5000) { *why = "window_ms must be 50..5000"; return 400; }
    if (argc - 3 > MAXCHG) { *why = "at most 4 changes per experiment"; return 400; }
    if (!(a->caps & (1u << CAP_TUNE))) { *why = "experiments need the tune capability too"; return 403; }
    for (int i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { *why = "changes are name=value"; return 400; }
        char nm[40]; size_t l = (size_t)(eq - argv[i]); if (l >= sizeof nm) l = sizeof nm - 1;
        memcpy(nm, argv[i], l); nm[l] = 0;
        int r = tune_check_one(a, nm, eq + 1, why);
        if (r) return r;
        if (tune_find(nm)->flags & TF_BOOT) { *why = "boot-time settings cannot be trialled live"; return 400; }
    }
    return 0;
}
static int t_exp(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    i64 w, base, trial; num(argv[2], &w);
    char olds[MAXCHG][64], nm[MAXCHG][40], nv[MAXCHG][48]; int n = argc - 3;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(argv[3 + i], '=');
        size_t l = (size_t)(eq - argv[3 + i]); if (l >= sizeof nm[0]) l = sizeof nm[0] - 1;
        memcpy(nm[i], argv[3 + i], l); nm[i][l] = 0; strlcpy(nv[i], eq + 1, sizeof nv[0]);
        tune_format(tune_find(nm[i]), olds[i], sizeof olds[0]);
    }
    metric_reset(); thread_sleep_ms((u64)w); metric_value(argv[1], &base);
    const char *m = "";
    for (int i = 0; i < n; i++) tune_set(nm[i], nv[i], &m);
    metric_reset(); thread_sleep_ms((u64)w); metric_value(argv[1], &trial);
    extern struct tunable *t_gain;
    int keep = trial * 100 < base * (100 - t_gain->val);       /* must beat the baseline by more than agent.min_gain_pct */
    if (keep) for (int i = 0; i < n; i++) journal_push(a, J_TUNE, 0, nm[i], olds[i], NULL, 0, 1);
    else for (int i = n - 1; i >= 0; i--) tune_set(nm[i], olds[i], &m);
    jp(o, "{\"metric\":\"%s\",\"baseline\":%ld,\"trial\":%ld,\"kept\":%s,\"changes\":[", argv[1], base, trial, keep ? "true" : "false");
    for (int i = 0; i < n; i++) { jp(o, "%s{\"name\":\"%s\",\"old\":\"%s\",\"new\":\"%s\"}", i ? "," : "", nm[i], olds[i], nv[i]); }
    jp(o, "]}");
    return 0;
}

static int t_rollback(struct agent *a, int argc, char **argv, struct jbuf *o)
{
    i64 n = 1; if (argc > 1) num(argv[1], &n);
    jp(o, "{\"undone\":");
    int d = journal_rollback(a, (int)n, o);
    (void)d;
    jp(o, "}");
    return 0;
}

/* ------------------------------------------------------------ table */
const struct tool agent_tools[] = {
    { "whoami",          "",                                  "your identity, capabilities and approval policy",        CAP_OBSERVE, RISK_READ, NULL, t_whoami },
    { "tools",           "",                                  "tools you may call, with parameters and risk class",      CAP_OBSERVE, RISK_READ, NULL, t_tools },
    { "sys.info",        "",                                  "OS, uptime, CPU, memory, thread counts",                  CAP_OBSERVE, RISK_READ, NULL, t_sysinfo },
    { "mod.list",        "",                                  "kernel modules and how many settings each exposes",       CAP_OBSERVE, RISK_READ, NULL, t_modlist },
    { "thread.list",     "",                                  "threads with priority, CPU share and wake latency",       CAP_OBSERVE, RISK_READ, NULL, t_threads },
    { "mem.info",        "",                                  "physical memory, kernel heap, filesystem usage",          CAP_OBSERVE, RISK_READ, NULL, t_meminfo },
    { "rt.stats",        "",                                  "tick jitter, real-time wake latency, tick-handler cost",  CAP_OBSERVE, RISK_READ, NULL, t_rtstats },
    { "net.info",        "",                                  "network addresses",                                        CAP_OBSERVE, RISK_READ, NULL, t_netinfo },
    { "tune.list",       "prefix?:str,offset?:int",           "list settings (12 per call) with type, range, mode",      CAP_OBSERVE, RISK_READ, NULL, t_tunelist },
    { "tune.get",        "name:str",                          "read one setting",                                         CAP_OBSERVE, RISK_READ, NULL, t_tuneget },
    { "var.list",        "",                                  "shared variables in Linux and Windows form",              CAP_OBSERVE, RISK_READ, NULL, t_varlist },
    { "var.get",         "name:str,view?:lin|win",            "read a shared variable in a world's convention",          CAP_OBSERVE, RISK_READ, NULL, t_varget },
    { "audit.tail",      "n?:int",                            "latest audit records (all agents)",                       CAP_OBSERVE, RISK_READ, NULL, t_audit },
    { "pkg.list",        "",                                  "packages in the loaded index",                             CAP_OBSERVE, RISK_READ, NULL, t_pkglist },
    { "rollback",        "n?:int",                            "undo your last n journaled changes",                       CAP_OBSERVE, RISK_REV,  NULL, t_rollback },
    { "tune.set",        "name:str,value:str",                "change a setting (live, or pending for next boot)",       CAP_TUNE,    RISK_REV,  c_tuneset, t_tuneset },
    { "var.set",         "name:str,value:str,view?:lin|win",  "set a shared variable; NEOS.* are the settings",         CAP_VARS,    RISK_REV,  c_varset, t_varset },
    { "var.unset",       "name:str",                          "remove a shared variable",                                 CAP_VARS,    RISK_REV,  c_varunset, t_varunset },
    { "thread.renice",   "id:int,prio:int",                   "change a non-core thread's priority within your band",     CAP_THREAD,  RISK_REV,  c_renice, t_renice },
    { "thread.kill",     "id:int",                            "stop a non-core thread within your band",                  CAP_THREAD,  RISK_IRREV, c_kill, t_kill },
    { "fs.ls",           "dir:str",                           "list a directory",                                         CAP_FSR,     RISK_READ, c_fsls, t_fsls },
    { "fs.read",         "path:str,offset?:int,len?:int",     "read up to 768 bytes (base64) from a file",               CAP_FSR,     RISK_READ, c_fsread, t_fsread },
    { "fs.write",        "path:str,data_b64:str,mode?:append","write/append up to 2 KiB (base64) to a file",             CAP_FSW,     RISK_REV,  c_fswrite, t_fswrite },
    { "fs.rm",           "path:str",                          "delete a file",                                            CAP_FSW,     RISK_IRREV, c_fsread, t_fsrm },
    { "pkg.update",      "",                                  "refresh the package index from the repository",            CAP_PKG,     RISK_REV,  NULL, t_pkgupdate },
    { "pkg.install",     "name:str",                          "download, verify and install a package",                   CAP_PKG,     RISK_REV,  c_pkgname, t_pkginstall },
    { "pkg.remove",      "name:str",                          "remove an installed package",                              CAP_PKG,     RISK_IRREV, c_pkgname, t_pkgremove },
    { "pkg.build",       "name:str,version:str,dir:str",      "pack a directory of the filesystem into an NPK package",   CAP_PKG,     RISK_REV,  c_pkgbuild, t_pkgbuild },
    { "pkg.install_file","name:str,version:str,path:str",     "install a locally built NPK package",                     CAP_PKG,     RISK_REV,  c_pkginstfile, t_pkginstfile },
    { "net.get",         "url:str,dest:str",                  "HTTP GET into a file (host must be in your net scope)",   CAP_NET,     RISK_REV,  c_netget, t_netget },
    { "debug.peek",      "addr:int,len:int",                  "read up to 128 bytes of kernel memory (read-only)",       CAP_DEBUG,   RISK_READ, c_peek, t_peek },
    { "experiment.run",  "metric:str,window_ms:int,changes:name=value...", "apply settings, measure, keep only if better, else revert", CAP_EXP, RISK_REV, c_exp, t_exp },
};
const int agent_ntools = (int)(sizeof agent_tools / sizeof agent_tools[0]);
