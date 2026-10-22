#include "varbus.h"

#define MAX_VARS   96
#define MAX_PROV   16
#define MAX_SUBS   8
#define QN         16
#define VLEN       256
#define EVN        16

struct var { char name[40]; u8 type, view; unsigned ver; char val[VLEN]; int used; };
static struct var vars[MAX_VARS];

struct msg { int state; char name[40]; char text[VLEN]; u8 view, op; int result; const char *err; int async; };
enum { M_FREE, M_QUEUED, M_DONE };
static struct msg q[QN];

static struct { char prefix[40]; bus_cb cb; } subs[MAX_SUBS];
static int nsubs;
static struct { char name[40]; bus_provider_fn fn; } prov[MAX_PROV];
static int nprov;
static struct { char name[40]; unsigned ver; u8 view; u64 tick; } ev[EVN];
static unsigned ev_head, ev_count;
static struct tunable *t_drive, *t_root, *t_prio;

static const struct { const char *canon, *alias; } aliases[] = {
    { "HOME", "USERPROFILE" }, { "USER", "USERNAME" }, { "TMPDIR", "TEMP" }, { "TMPDIR", "TMP" },
    { "HOSTNAME", "COMPUTERNAME" }, { "PWD", "CD" }, { "SHELL", "COMSPEC" }, { "PATH", "Path" },
};
static const struct { const char *name; int type; } known[] = {
    { "PATH", VT_PLIST }, { "HOME", VT_PATH }, { "TMPDIR", VT_PATH }, { "PWD", VT_PATH }, { "SHELL", VT_PATH },
};

const char *bus_type_name(int t)
{
    static const char *n[] = { "str", "int", "bool", "path", "pathlist" };
    return t >= 0 && t <= VT_PLIST ? n[t] : "?";
}

/* ------------------------------------------------------------ path translation */
static void put(char *o, size_t n, size_t *l, char c) { if (*l + 1 < n) o[*l] = c; (*l)++; }
static void puts_(char *o, size_t n, size_t *l, const char *s) { while (*s) put(o, n, l, *s++); }

int path_win_to_posix(const char *w, char *o, size_t n)
{
    size_t l = 0;
    if (w[0] == '\\' && w[1] == '\\') {
        const char *r = w + 2;
        if (!strncmp(r, "neos\\root", 9) && (r[9] == '\\' || !r[9])) { r += 9; if (!*r) put(o, n, &l, '/'); }
        else puts_(o, n, &l, "/mnt/unc/");
        for (; *r; r++) put(o, n, &l, *r == '\\' ? '/' : *r);
    } else if (is_alpha(w[0]) && w[1] == ':') {
        puts_(o, n, &l, "/mnt/"); put(o, n, &l, (char)to_lower(w[0]));
        const char *r = w + 2;
        if (*r != '\\' && *r != '/' && *r) put(o, n, &l, '/');
        for (; *r; r++) put(o, n, &l, *r == '\\' ? '/' : *r);
    } else if (w[0] == '\\') {
        puts_(o, n, &l, "/mnt/"); puts_(o, n, &l, t_drive ? t_drive->sval : "c");
        for (const char *r = w; *r; r++) put(o, n, &l, *r == '\\' ? '/' : *r);
    } else for (const char *r = w; *r; r++) put(o, n, &l, *r == '\\' ? '/' : *r);
    if (n) o[l < n ? l : n - 1] = 0;
    return (int)l;
}

int path_posix_to_win(const char *p, char *o, size_t n)
{
    size_t l = 0;
    if (!strncmp(p, "/mnt/unc/", 9)) { puts_(o, n, &l, "\\\\"); p += 9; }
    else if (!strncmp(p, "/mnt/", 5) && is_alpha(p[5]) && (p[6] == '/' || !p[6])) {
        put(o, n, &l, (char)to_upper(p[5])); put(o, n, &l, ':');
        p += 6; if (!*p) put(o, n, &l, '\\');
    } else if (p[0] == '/') {
        puts_(o, n, &l, t_root ? t_root->sval : "\\\\neos\\root");
        if (!p[1]) { put(o, n, &l, '\\'); p++; }
    }
    for (; *p; p++) put(o, n, &l, *p == '/' ? '\\' : *p);
    if (n) o[l < n ? l : n - 1] = 0;
    return (int)l;
}

/* path lists: canonical form joins items with '\n' */
static void plist_parse(const char *s, int view, char *o, size_t n)
{
    char sep = view == NEOS_VIEW_WINDOWS ? ';' : ':', item[VLEN];
    size_t l = 0;
    while (*s) {
        size_t k = 0;
        while (s[k] && s[k] != sep) k++;
        if (k && k < sizeof item) {
            memcpy(item, s, k); item[k] = 0;
            if (l) put(o, n, &l, '\n');
            char c[VLEN];
            if (view == NEOS_VIEW_WINDOWS) path_win_to_posix(item, c, sizeof c); else strlcpy(c, item, sizeof c);
            puts_(o, n, &l, c);
        }
        s += k; if (*s) s++;
    }
    if (n) o[l < n ? l : n - 1] = 0;
}

static void plist_render(const char *canon, int view, char *o, size_t n)
{
    size_t l = 0; char item[VLEN];
    while (*canon) {
        size_t k = 0;
        while (canon[k] && canon[k] != '\n') k++;
        if (k < sizeof item) {
            memcpy(item, canon, k); item[k] = 0;
            if (l) put(o, n, &l, view == NEOS_VIEW_WINDOWS ? ';' : ':');
            char c[VLEN];
            if (view == NEOS_VIEW_WINDOWS) path_posix_to_win(item, c, sizeof c); else strlcpy(c, item, sizeof c);
            puts_(o, n, &l, c);
        }
        canon += k; if (*canon) canon++;
    }
    if (n) o[l < n ? l : n - 1] = 0;
}

/* ------------------------------------------------------------ naming */
static const char *canon_name(const char *name)
{
    for (unsigned i = 0; i < sizeof aliases / sizeof *aliases; i++)
        if (!strcasecmp(name, aliases[i].alias)) return aliases[i].canon;
    return name;
}

static struct var *find(const char *name, int view)
{
    name = canon_name(name);
    for (int i = 0; i < MAX_VARS; i++) if (vars[i].used && !strcmp(vars[i].name, name)) return &vars[i];
    if (view == NEOS_VIEW_WINDOWS)      /* Windows names are case-insensitive */
        for (int i = 0; i < MAX_VARS; i++) if (vars[i].used && !strcasecmp(vars[i].name, name)) return &vars[i];
    return NULL;
}

static int looks_win_path(const char *s)
{
    return (is_alpha(s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/' || !s[2])) || (s[0] == '\\' && s[1] == '\\');
}

static int guess_type(const char *name, const char *text, int view)
{
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++) if (!strcmp(name, known[i].name)) return known[i].type;
    i64 v;
    if (!parse_i64(text, &v)) return VT_INT;
    if (!strcasecmp(text, "true") || !strcasecmp(text, "false")) return VT_BOOL;
    if (view == NEOS_VIEW_WINDOWS) {
        if (looks_win_path(text)) return strchr(text + 2, ';') ? VT_PLIST : VT_PATH;
        if (strchr(text, ';') && looks_win_path(strchr(text, ';') + 1)) return VT_PLIST;
    } else {
        if (text[0] == '/') return strchr(text, ':') && strchr(text, ':')[1] == '/' ? VT_PLIST : VT_PATH;
    }
    return VT_STR;
}

/* text in `view` convention -> canonical; 0 ok */
static int to_canon(int type, const char *text, int view, char *o, size_t n, const char **err)
{
    i64 v;
    switch (type) {
    case VT_INT:  if (parse_i64(text, &v)) { *err = "expected an integer"; return -1; } ksnprintf(o, n, "%ld", v); return 0;
    case VT_BOOL:
        if (!strcasecmp(text, "1") || !strcasecmp(text, "true") || !strcasecmp(text, "on") || !strcasecmp(text, "yes")) { strlcpy(o, "1", n); return 0; }
        if (!strcasecmp(text, "0") || !strcasecmp(text, "false") || !strcasecmp(text, "off") || !strcasecmp(text, "no")) { strlcpy(o, "0", n); return 0; }
        *err = "expected a boolean"; return -1;
    case VT_PATH: if (view == NEOS_VIEW_WINDOWS) path_win_to_posix(text, o, n); else strlcpy(o, text, n); return 0;
    case VT_PLIST: plist_parse(text, view, o, n); return 0;
    default: strlcpy(o, text, n); return 0;
    }
}

static void render(const struct var *v, int view, char *o, size_t n)
{
    if (v->type == VT_PATH && view == NEOS_VIEW_WINDOWS) path_posix_to_win(v->val, o, n);
    else if (v->type == VT_PLIST) plist_render(v->val, view, o, n);
    else strlcpy(o, v->val, n);
}

/* ------------------------------------------------------------ queue + interpreter thread */
static void log_event(const char *name, unsigned ver, int view)
{
    unsigned i = ev_head++ % EVN;
    strlcpy(ev[i].name, name, sizeof ev[i].name); ev[i].ver = ver; ev[i].view = (u8)view; ev[i].tick = ticks;
    if (ev_count < EVN) ev_count++;
}

static int apply_set(struct msg *m)
{
    const char *cn = canon_name(m->name), *err = "";
    if (!strncmp(cn, "NEOS.", 5)) {                            /* virtual namespace: live tunables */
        int r = tune_set(cn + 5, m->text, &err);
        m->err = err; log_event(cn, 0, m->view);
        return r < 0 ? -1 : 0;
    }
    struct var *v = find(m->name, m->view);
    int type = v ? v->type : guess_type(cn, m->text, m->view);
    char c[VLEN];
    if (to_canon(type, m->text, m->view, c, sizeof c, &err)) { m->err = err; return -1; }
    u64 fl = irq_save();
    if (!v) {
        for (int i = 0; i < MAX_VARS; i++) if (!vars[i].used) { v = &vars[i]; break; }
        if (!v) { irq_restore(fl); m->err = "variable table full"; return -1; }
        memset(v, 0, sizeof *v); v->used = 1; v->type = (u8)type;
        strlcpy(v->name, cn, sizeof v->name);
    }
    strlcpy(v->val, c, sizeof v->val);
    v->view = m->view; v->ver++;
    unsigned ver = v->ver;
    irq_restore(fl);
    log_event(v->name, ver, m->view);
    for (int i = 0; i < nsubs; i++) if (!strncmp(v->name, subs[i].prefix, strlen(subs[i].prefix))) subs[i].cb(v->name, c);
    return 0;
}

static void bus_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int worked = 0;
        for (int i = 0; i < QN; i++) {
            struct msg *m = &q[i];
            if (m->state != M_QUEUED) continue;
            worked = 1;
            if (m->op == 0) m->result = apply_set(m);
            else { struct var *v = find(m->name, m->view); if (v) { v->used = 0; log_event(v->name, v->ver + 1, m->view); m->result = 0; } else { m->err = "no such variable"; m->result = -1; } }
            if (m->async) m->state = M_FREE; else m->state = M_DONE;
        }
        if (!worked) thread_sleep_until(ticks + 1);
    }
}

void bus_init(void)
{
    module_register("bus", "1.0", "Linux<->Windows variable interpreter (canonical store, per-world views)");
    t_drive = tune_str("interop", "default_drive", "drive letter for rooted Windows paths without a drive", TF_LIVE, "c", NULL);
    t_root  = tune_str("interop", "linux_root", "how the Linux root appears to Windows programs", TF_LIVE, "\\\\neos\\root", NULL);
    t_prio  = tune_int("bus", "prio", "priority of the interpreter thread", TF_BOOT, 3, 0, 6, NULL);
}

void bus_start(void) { thread_create("varbus", (u8)t_prio->val, bus_thread, NULL); }

static struct msg *alloc_msg(void)
{
    u64 fl = irq_save();
    for (int i = 0; i < QN; i++) if (q[i].state == M_FREE) { q[i].state = 3; irq_restore(fl); return &q[i]; }
    irq_restore(fl);
    return NULL;
}

static int submit(int op, const char *name, const char *text, int view, int sync, const char **err)
{
    static const char *full = "interpreter queue full";
    struct msg *m = alloc_msg();
    if (!m) { if (err) *err = full; return -1; }
    strlcpy(m->name, name, sizeof m->name);
    strlcpy(m->text, text ? text : "", sizeof m->text);
    m->view = (u8)view; m->op = (u8)op; m->async = !sync; m->err = ""; m->result = 0;
    m->state = M_QUEUED;
    if (!sync) return 0;
    for (u64 waited = 0; m->state != M_DONE; waited++) {
        thread_sleep_until(ticks + 1);
        if (waited > 2000) { if (err) *err = "interpreter timeout"; return -1; }
    }
    int r = m->result;
    if (err) *err = m->err;
    m->state = M_FREE;
    return r;
}

int  bus_set(const char *name, const char *text, int view, int sync, const char **err) { return submit(0, name, text, view, sync, err); }
int  bus_unset(const char *name, int view) { const char *e; return submit(1, name, NULL, view, 1, &e); }
long bus_post_set(const char *name, const char *value, int view) { return bus_set(name, value, view, 0, NULL); }

void bus_declare(const char *name, int type)
{
    struct var *v = find(name, NEOS_VIEW_LINUX);
    if (v) { v->type = (u8)type; return; }
    const char *e; bus_set(name, "", NEOS_VIEW_NATIVE, 1, &e);
    if ((v = find(name, NEOS_VIEW_LINUX))) v->type = (u8)type;
}

void bus_subscribe(const char *prefix, bus_cb cb)
{
    if (nsubs < MAX_SUBS) { strlcpy(subs[nsubs].prefix, prefix, sizeof subs[nsubs].prefix); subs[nsubs].cb = cb; nsubs++; }
}
void bus_provider(const char *name, bus_provider_fn fn)
{
    if (nprov < MAX_PROV) { strlcpy(prov[nprov].name, name, sizeof prov[nprov].name); prov[nprov].fn = fn; nprov++; }
}

/* ------------------------------------------------------------ reads */
long bus_get(const char *name, int view, char *buf, unsigned long len)
{
    const char *cn = canon_name(name);
    if (!strncmp(cn, "NEOS.", 5)) {
        for (int i = 0; i < nprov; i++) if (!strcmp(prov[i].name, cn + 5)) { prov[i].fn(buf, len); return (long)strlen(buf); }
        struct tunable *t = tune_find(cn + 5);
        if (!t) return -1;
        tune_format(t, buf, len);
        return (long)strlen(buf);
    }
    struct var *v = find(name, view);
    if (!v) return -1;
    u64 fl = irq_save();
    struct var copy = *v;
    irq_restore(fl);
    render(&copy, view, buf, len);
    return (long)strlen(buf);
}

int bus_count(void) { int n = 0; for (int i = 0; i < MAX_VARS; i++) n += vars[i].used; return n; }
int bus_info(int idx, struct bus_var_info *o)
{
    for (int i = 0, k = 0; i < MAX_VARS; i++) if (vars[i].used && k++ == idx) {
        strlcpy(o->name, vars[i].name, sizeof o->name); o->type = vars[i].type; o->view = vars[i].view; o->ver = vars[i].ver;
        return 0;
    }
    return -1;
}

int bus_event(int i, char *name, size_t n, unsigned *ver, int *view, u64 *tick)
{
    if ((unsigned)i >= ev_count) return -1;
    unsigned k = (ev_head - 1 - i) % EVN;
    strlcpy(name, ev[k].name, n); *ver = ev[k].ver; *view = ev[k].view; *tick = ev[k].tick;
    return 0;
}

int bus_env_block(int view, char *buf, size_t n)
{
    size_t l = 0; char val[VLEN];
    for (int i = 0; i < MAX_VARS; i++) if (vars[i].used) {
        render(&vars[i], view, val, sizeof val);
        l += ksnprintf(buf + l, l < n ? n - l : 0, "%s=%s\n", vars[i].name, val);
    }
    return (int)l;
}

/* ------------------------------------------------------------ expansion */
static int name_char(int c) { return is_alpha(c) || is_digit(c) || c == '_' || c == '.'; }

void bus_expand(const char *in, char *out, size_t n)
{
    size_t l = 0;
    while (*in) {
        char c = *in;
        if (c == '$' && (in[1] == '{' || is_alpha(in[1]) || in[1] == '_')) {
            char name[48], def[64]; int view = NEOS_VIEW_LINUX, have_def = 0; size_t k = 0;
            const char *p = in + 1;
            if (*p == '{') {
                p++;
                while (*p && *p != '}' && *p != '@' && !(*p == ':' && p[1] == '-')) { if (k < sizeof name - 1) name[k++] = *p; p++; }
                if (*p == '@') { p++; view = (to_lower(*p) == 'w') ? NEOS_VIEW_WINDOWS : NEOS_VIEW_LINUX; while (*p && *p != '}' && *p != ':') p++; }
                if (*p == ':' && p[1] == '-') { p += 2; size_t d = 0; while (*p && *p != '}') { if (d < sizeof def - 1) def[d++] = *p; p++; } def[d] = 0; have_def = 1; }
                if (*p == '}') p++;
            } else while (is_alpha(*p) || is_digit(*p) || *p == '_') { if (k < sizeof name - 1) name[k++] = *p; p++; }
            name[k] = 0;
            char val[VLEN];
            if (bus_get(name, view, val, sizeof val) >= 0) puts_(out, n, &l, val);
            else if (have_def) puts_(out, n, &l, def);
            in = p; continue;
        }
        if (c == '%' && in[1] == '%') { put(out, n, &l, '%'); in += 2; continue; }
        if (c == '%' && (is_alpha(in[1]) || in[1] == '_')) {
            const char *p = in + 1; char name[48]; size_t k = 0; int view = NEOS_VIEW_WINDOWS;
            while (name_char(*p) && k < sizeof name - 1) name[k++] = *p++;
            if (*p == '@') { p++; view = (to_lower(*p) == 'l') ? NEOS_VIEW_LINUX : NEOS_VIEW_WINDOWS; while (is_alpha(*p)) p++; }
            name[k] = 0;
            char val[VLEN];
            if (*p == '%' && bus_get(name, view, val, sizeof val) >= 0) { puts_(out, n, &l, val); in = p + 1; continue; }
            /* cmd.exe semantics: an unknown %NAME% stays literal */
        }
        put(out, n, &l, c); in++;
    }
    if (n) out[l < n ? l : n - 1] = 0;
}
