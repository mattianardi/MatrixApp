#include "agent.h"
#include "fs.h"
#include "shell.h"
#include "varbus.h"

static char *strstr_(const char *s) { for (; *s; s++) if (s[0] == '.' && s[1] == '.') return (char *)s; return NULL; }
static int atoi_(const char *s) { i64 v = 0; parse_i64(s, &v); return (int)v; }
#define shell_tokenize_ shell_tokenize

const char *const cap_names[NCAPS] = { "observe", "tune", "thread", "vars", "fs.read", "fs.write", "pkg", "net", "debug", "experiment", "admin" };

static struct agent agents[AG_MAX];
struct tunable *t_gain;
static struct tunable *t_rate, *t_ask_s, *t_quota;
static int frozen_local;

/* ------------------------------------------------------------ json helpers */
void jp(struct jbuf *j, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (j->len + 1 < j->n) j->len += kvsnprintf(j->b + j->len, j->n - j->len, fmt, ap);
    else { char t[2]; kvsnprintf(t, 1, fmt, ap); }
    va_end(ap);
    if (j->len >= j->n) j->len = j->n - 1;
}

void jstr(struct jbuf *j, const char *s)
{
    jp(j, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') jp(j, "\\%c", *s);
        else if (*s == '\n') jp(j, "\\n");
        else if ((u8)*s < 32) jp(j, "\\u%04x", (u8)*s);
        else jp(j, "%c", *s);
    }
    jp(j, "\"");
}

/* ------------------------------------------------------------ registry */
void agent_init(void)
{
    module_register("agent", "1.0", "agents as kernel principals: capabilities, approvals, audit chain, journal, kill switch");
    t_rate  = tune_int("agent", "rate_per_min",   "max actions per minute per agent", TF_LIVE | TF_NOAGENT, 120, 1, 6000, NULL);
    t_ask_s = tune_int("agent", "approval_timeout_s", "how long an agent waits for the operator's decision", TF_LIVE | TF_NOAGENT, 60, 1, 3600, NULL);
    t_gain  = tune_int("agent", "min_gain_pct", "an experiment keeps a change only if the metric improves by more than this (guards against measurement noise)", TF_LIVE | TF_NOAGENT, 5, 0, 90, NULL);
    t_quota = tune_int("agent", "fs_quota_kib", "bytes an agent may leave in the filesystem (KiB)", TF_LIVE | TF_NOAGENT, 256, 1, 65536, NULL);
}

struct agent *agent_at(int i) { return i >= 0 && i < AG_MAX && agents[i].used ? &agents[i] : NULL; }

struct agent *agent_find(const char *name)
{
    for (int i = 0; i < AG_MAX; i++) if (agents[i].used && !strcmp(agents[i].name, name)) return &agents[i];
    return NULL;
}

int agent_cap_index(const char *name)
{
    for (int i = 0; i < NCAPS; i++) if (!strcmp(cap_names[i], name)) return i;
    return -1;
}

struct agent *agent_add(const char *name, int kind, const char *secret, char *token_out, size_t tn, const char **err)
{
    if (!name[0] || strlen(name) >= sizeof agents[0].name) { *err = "bad agent name (max 15 chars)"; return NULL; }
    if (agent_find(name)) { *err = "agent already exists"; return NULL; }
    struct agent *a = NULL;
    for (int i = 0; i < AG_MAX; i++) if (!agents[i].used) { a = &agents[i]; break; }
    if (!a) { *err = "agent table full"; return NULL; }
    memset(a, 0, sizeof *a);
    a->used = 1; a->kind = (u8)kind; a->state = AS_ACTIVE;
    strlcpy(a->name, name, sizeof a->name);
    a->policy[RISK_READ] = POL_ALLOW; a->policy[RISK_REV] = POL_ALLOW; a->policy[RISK_IRREV] = POL_ASK;
    a->tokens_ms = 60 * 1000;
    a->last_ms = time_ns() / 1000000;
    if (kind == AK_REMOTE) {
        char tok[48];
        if (secret && secret[0]) {
            if (strlen(secret) < 12) { a->used = 0; *err = "secret must be at least 12 characters"; return NULL; }
            strlcpy(tok, secret, sizeof tok);
            if (token_out) token_out[0] = 0;
        } else {
            u64 r1, r2;
            if (hw_random(&r1) || hw_random(&r2)) { a->used = 0; *err = "no hardware random source (RDRAND): supply secret=<12+ chars> instead"; return NULL; }
            ksnprintf(tok, sizeof tok, "%016lx%016lx", r1, r2);
            if (token_out) strlcpy(token_out, tok, tn);
        }
        sha256_hex(tok, strlen(tok), a->token_hash);
    }
    return a;
}

int agent_remove(const char *name)
{
    struct agent *a = agent_find(name);
    if (!a) return -1;
    for (int i = 0; i < a->jn; i++) if (a->jr[i].blob) kfree(a->jr[i].blob);
    a->used = 0;
    return 0;
}

int agent_authenticate(struct agent *a, const char *token)
{
    if (a->kind != AK_REMOTE || !a->token_hash[0]) return -1;
    char h[65]; sha256_hex(token, strlen(token), h);
    int diff = 0;
    for (int i = 0; i < 64; i++) diff |= h[i] ^ a->token_hash[i];       /* constant time */
    return diff ? -1 : 0;
}

int agent_grant(struct agent *a, const char *cap, const char *scope, const char **err)
{
    int ci = agent_cap_index(cap);
    if (ci < 0) { *err = "unknown capability"; return -1; }
    a->caps |= 1u << ci;
    if (scope) strlcpy(a->scope[ci], scope, sizeof a->scope[ci]);
    return 0;
}

int agent_revoke(struct agent *a, const char *cap)
{
    if (!cap) { a->caps = 0; memset(a->scope, 0, sizeof a->scope); return 0; }
    int ci = agent_cap_index(cap);
    if (ci < 0) return -1;
    a->caps &= ~(1u << ci); a->scope[ci][0] = 0;
    return 0;
}

void agent_freeze(void) { frozen_local = 1; agents_frozen = 1; }
void agent_thaw(void) { frozen_local = 0; agents_frozen = 0; }
int agent_is_frozen(void) { return frozen_local || agents_frozen; }

/* ------------------------------------------------------------ scopes */
/* items: "*", "prefix", or "name[lo..hi]" (integer range for the value) */
int scope_allows(const struct agent *a, int cap, const char *target, int has_val, i64 val)
{
    const char *s = a->scope[cap];
    while (*s) {
        char item[48]; size_t k = 0;
        while (*s && *s != ',' && k < sizeof item - 1) item[k++] = *s++;
        item[k] = 0;
        if (*s == ',') s++;
        char *br = strchr(item, '[');
        i64 lo = 0, hi = 0; int range = 0;
        if (br) {
            *br = 0;
            char *dots = strstr_(br + 1), *close = strchr(br + 1, ']');
            if (close) *close = 0;
            if (dots && close) { *dots = 0; if (!parse_i64(br + 1, &lo) && !parse_i64(dots + 2, &hi)) range = 1; }
            if (!range) continue;                              /* malformed range: fail closed, grant nothing */
        }
        if (!strcmp(item, "*") || !strncmp(target, item, strlen(item))) {
            if (range && has_val && (val < lo || val > hi)) continue;
            return 1;
        }
    }
    return 0;
}

int scope_minprio(const struct agent *a)
{
    const char *p = a->scope[CAP_THREAD];
    i64 v;
    if (!strncmp(p, "minprio=", 8) && !parse_i64(p + 8, &v)) return (int)v;
    return 7;
}

/* ------------------------------------------------------------ audit (hash chain) */
#define AUDN 256
static struct audit_rec aud[AUDN];
static u64 aud_seq;

static void aud_hash(const struct audit_rec *r, const char *prev, char out[65])
{
    char buf[256];
    int n = ksnprintf(buf, sizeof buf, "%s|%lu|%lu|%u|%s|%s|%d|%u", prev, r->seq, r->tick, r->agent, r->tool, r->args, r->rc, r->approver);
    sha256_hex(buf, (size_t)n, out);
}

void audit_add(int agent, const char *tool, const char *args, int rc, int approver)
{
    u64 fl = irq_save();
    struct audit_rec *r = &aud[aud_seq % AUDN];
    const char *prev = aud_seq ? aud[(aud_seq - 1) % AUDN].hash : "0000000000000000000000000000000000000000000000000000000000000000";
    r->seq = aud_seq + 1; r->tick = ticks; r->agent = (u8)agent; r->rc = rc; r->approver = (u8)approver;
    strlcpy(r->tool, tool, sizeof r->tool); strlcpy(r->args, args, sizeof r->args);
    char hp[65]; strlcpy(hp, prev, sizeof hp);
    aud_hash(r, hp, r->hash);
    aud_seq++;
    irq_restore(fl);
}

int audit_count(void) { return aud_seq < AUDN ? (int)aud_seq : AUDN; }
int audit_get(int back, struct audit_rec *o)
{
    if (back < 0 || back >= audit_count()) return -1;
    *o = aud[(aud_seq - 1 - back) % AUDN];
    return 0;
}

int audit_verify(void)
{
    int n = audit_count();
    for (int i = n - 1; i >= 0; i--) {
        struct audit_rec *r = &aud[(aud_seq - 1 - i) % AUDN];
        char prev[65], h[65];
        if (i == n - 1 && aud_seq > AUDN) continue;              /* oldest surviving record: its predecessor is gone */
        if (r->seq == 1) strlcpy(prev, "0000000000000000000000000000000000000000000000000000000000000000", sizeof prev);
        else strlcpy(prev, aud[(r->seq - 2) % AUDN].hash, sizeof prev);
        aud_hash(r, prev, h);
        if (strcmp(h, r->hash)) return (int)r->seq;
    }
    return 0;
}

/* ------------------------------------------------------------ approvals */
#define APN 8
static struct approval aps[APN];
static int ap_next = 1;
enum { AP_FREE, AP_PENDING, AP_ALLOWED, AP_DENIED };

int approval_count_pending(void) { int n = 0; for (int i = 0; i < APN; i++) n += aps[i].state == AP_PENDING; return n; }
int approval_get(int idx, struct approval *o)
{
    for (int i = 0, k = 0; i < APN; i++) if (aps[i].state == AP_PENDING && k++ == idx) { *o = aps[i]; return 0; }
    return -1;
}
int approval_decide(int id, int allow)
{
    for (int i = 0; i < APN; i++) if (aps[i].state == AP_PENDING && aps[i].id == id) { aps[i].state = allow ? AP_ALLOWED : AP_DENIED; return 0; }
    return -1;
}

static int ask_operator(struct agent *a, const char *tool, const char *args)
{
    struct approval *ap = NULL;
    u64 fl = irq_save();
    for (int i = 0; i < APN; i++) if (aps[i].state == AP_FREE) { ap = &aps[i]; break; }
    if (ap) { ap->id = ap_next++; ap->agent = (int)(a - agents); strlcpy(ap->tool, tool, sizeof ap->tool); strlcpy(ap->args, args, sizeof ap->args); ap->tick = ticks; ap->state = AP_PENDING; }
    irq_restore(fl);
    if (!ap) return 0;
    a->asked++;
    kprintf("\n\x1b[1;93m[agent]\x1b[0m %s asks to run: \x1b[1m%s %s\x1b[0m   ->  \x1b[1magent approve %d\x1b[0m | \x1b[1magent deny %d\x1b[0m\n", a->name, tool, args, ap->id, ap->id);
    u64 until = ticks + ms_to_ticks((u64)t_ask_s->val * 1000);
    while (ap->state == AP_PENDING && ticks < until && !agent_is_frozen()) thread_sleep_ms(50);
    int ok = ap->state == AP_ALLOWED;
    ap->state = AP_FREE;
    return ok;
}

/* ------------------------------------------------------------ journal */
void journal_push(struct agent *a, u8 kind, int ai, const char *name, const char *old, const u8 *blob, size_t blen, int had)
{
    if (a->jn == AG_JOURNAL) {                                   /* drop the oldest */
        if (a->jr[0].blob) kfree(a->jr[0].blob);
        memmove(&a->jr[0], &a->jr[1], sizeof a->jr[0] * (AG_JOURNAL - 1));
        a->jn--;
    }
    struct jent *e = &a->jr[a->jn++];
    memset(e, 0, sizeof *e);
    e->kind = kind; e->a = ai; e->had = had;
    strlcpy(e->name, name, sizeof e->name); if (old) strlcpy(e->old, old, sizeof e->old);
    if (blob && blen) { e->blob = kmalloc(blen); if (e->blob) { memcpy(e->blob, blob, blen); e->blen = blen; } }
}

int journal_rollback(struct agent *a, int n, struct jbuf *out)
{
    int done = 0;
    jp(out, "[");
    while (a->jn > 0 && done < n) {
        struct jent *e = &a->jr[--a->jn];
        const char *m = "";
        const char *kind = "?";
        switch (e->kind) {
        case J_TUNE: kind = "tune"; tune_set(e->name, e->old, &m); break;
        case J_VAR:  kind = "var"; if (e->had) { m = ""; bus_set(e->name, e->old, 1, 1, &m); } else bus_unset(e->name, 1); break;
        case J_PRIO: kind = "prio"; thread_set_prio(e->a, atoi_(e->old)); break;
        case J_FS:   kind = "fs"; if (e->had) fs_write(e->name, e->blob ? e->blob : (const u8 *)"", e->blen); else fs_remove(e->name); break;
        case J_PKG:  kind = "pkg"; if (e->had) { /* previous version not kept */ } else pkg_remove(e->name, &m); break;
        }
        jp(out, "%s{\"undone\":\"%s\",\"target\":", done ? "," : "", kind); jstr(out, e->name); jp(out, "}");
        if (e->blob) kfree(e->blob);
        done++;
    }
    jp(out, "]");
    return done;
}

/* ------------------------------------------------------------ the gate */
static int rate_ok(struct agent *a)
{
    u64 now = time_ns() / 1000000, cap = (u64)t_rate->val * 1000;           /* milli-tokens */
    u64 add = (now - a->last_ms) * (u64)t_rate->val / 60;                   /* rate/min -> milli-tokens per ms */
    a->last_ms = now;
    a->tokens_ms += add; if (a->tokens_ms > cap) a->tokens_ms = cap;
    if (a->tokens_ms < 1000) return 0;
    a->tokens_ms -= 1000;
    return 1;
}

static void deny(struct jbuf *out, const char *m) { out->len = 0; jp(out, "%s", m); }

int agent_call(struct agent *a, int argc, char **argv, struct jbuf *out, int operator_approved)
{
    char args[56] = ""; int rc, approver = 0;
    out->len = 0; out->b[0] = 0;
    if (argc < 1) { deny(out, "empty call"); return 400; }
    for (int i = 1; i < argc; i++) { if (i > 1) strlcat(args, " ", sizeof args); strlcat(args, argv[i], sizeof args); }
    int ai = (int)(a - agents);
    const struct tool *t = NULL;
    for (int i = 0; i < agent_ntools; i++) if (!strcmp(agent_tools[i].name, argv[0])) t = &agent_tools[i];

    if (agent_is_frozen()) { deny(out, "agents are frozen by the operator"); rc = 423; goto done; }
    if (a->state != AS_ACTIVE) { deny(out, "agent is suspended"); rc = 423; goto done; }
    if (!t) { deny(out, "unknown tool (see `tools`)"); rc = 404; goto done; }
    if (!(a->caps & (1u << t->cap))) { deny(out, "missing capability"); rc = 403; goto done; }
    if (!rate_ok(a)) { deny(out, "rate limit exceeded"); rc = 429; goto done; }
    const char *why = "out of scope";
    int ck = t->check ? t->check(a, argc, argv, &why) : 0;
    if (ck) { deny(out, why); rc = ck; goto done; }
    int pol = a->policy[t->risk];
    if (pol == POL_DENY) { deny(out, "denied by policy"); rc = 403; goto done; }
    if (pol == POL_ASK && !operator_approved) {
        if (!ask_operator(a, t->name, args)) { deny(out, "operator did not approve"); rc = 403; goto done; }
        approver = 1;
    } else if (operator_approved) approver = 1;
    rc = t->run(a, argc, argv, out);
    a->calls++;
done:
    if (rc == 403 || rc == 423 || rc == 429) a->denied++;
    audit_add(ai, argc ? argv[0] : "-", args, rc, approver);
    return rc;
}

int agent_exec_line(struct agent *a, char *line, struct jbuf *out, int operator_approved)
{
    char *argv[12];
    int argc = shell_tokenize_(line, argv, 12);
    return agent_call(a, argc, argv, out, operator_approved);
}

int agent_index(const struct agent *a) { return (int)(a - agents); }
