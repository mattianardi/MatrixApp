#include "kernel.h"

#define MAX_TUNABLES 160
#define MAX_BOOTARGS 64

static struct tunable tab[MAX_TUNABLES];
static int ntab;
static struct { char name[40]; char val[48]; } bootarg[MAX_BOOTARGS];
static int nboot;

static const char *bootarg_find(const char *name)
{
    for (int i = nboot - 1; i >= 0; i--) if (!strcmp(bootarg[i].name, name)) return bootarg[i].val;
    return NULL;
}

void tune_bootarg_add(const char *name, const char *val)
{
    for (int i = 0; i < nboot; i++)
        if (!strcmp(bootarg[i].name, name)) { strlcpy(bootarg[i].val, val, sizeof bootarg[i].val); return; }
    if (nboot == MAX_BOOTARGS) return;
    strlcpy(bootarg[nboot].name, name, sizeof bootarg[nboot].name);
    strlcpy(bootarg[nboot].val, val, sizeof bootarg[nboot].val);
    nboot++;
}

void tune_bootargs(const char *cmdline)
{
    char tok[96];
    while (cmdline && *cmdline) {
        while (*cmdline == ' ') cmdline++;
        size_t n = 0;
        while (cmdline[n] && cmdline[n] != ' ') n++;
        if (n && n < sizeof tok) {
            memcpy(tok, cmdline, n); tok[n] = 0;
            if (!strncmp(tok, "neos.", 5)) {
                char *eq = strchr(tok, '=');
                if (eq) { *eq = 0; tune_bootarg_add(tok + 5, eq + 1); }
            }
        }
        cmdline += n;
    }
}

static int parse_bool(const char *s, i64 *v)
{
    if (!strcasecmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "on") || !strcasecmp(s, "yes")) { *v = 1; return 0; }
    if (!strcasecmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "off") || !strcasecmp(s, "no")) { *v = 0; return 0; }
    return -1;
}

/* validate text for t; fills *iv / sv */
static int parse_for(const struct tunable *t, const char *text, i64 *iv, const char **msg)
{
    if (t->type == TT_STR) { if (strlen(text) >= sizeof t->sval) { *msg = "string too long"; return -1; } return 0; }
    if (t->type == TT_BOOL) { if (parse_bool(text, iv)) { *msg = "expected on/off"; return -1; } return 0; }
    if (parse_i64(text, iv)) { *msg = "expected a number"; return -1; }
    if (*iv < t->min || *iv > t->max) { *msg = "out of range"; return -1; }
    return 0;
}

static struct tunable *alloc(const char *mod, const char *name, const char *desc, u8 type, u8 flags, tune_cb cb)
{
    if (ntab == MAX_TUNABLES) panic("tunable table full");
    struct tunable *t = &tab[ntab++];
    ksnprintf(t->name, sizeof t->name, "%s.%s", mod, name);
    t->module = mod; t->desc = desc; t->type = type; t->flags = flags; t->cb = cb;
    return t;
}

/* a boot argument (or saved config) overrides the default before the module reads it */
static void apply_bootarg(struct tunable *t)
{
    const char *b = bootarg_find(t->name), *msg;
    i64 iv = 0;
    if (!b || (t->flags & TF_RO)) return;
    if (parse_for(t, b, &iv, &msg)) { kprintf("[tune] ignoring boot arg %s=%s (%s)\n", t->name, b, msg); return; }
    if (t->type == TT_STR) strlcpy(t->sval, b, sizeof t->sval); else t->val = iv;
}

struct tunable *tune_int(const char *mod, const char *name, const char *desc, u8 flags, i64 def, i64 min, i64 max, tune_cb cb)
{
    struct tunable *t = alloc(mod, name, desc, TT_INT, flags, cb);
    t->val = t->def = def; t->min = min; t->max = max;
    apply_bootarg(t);
    return t;
}
struct tunable *tune_bool(const char *mod, const char *name, const char *desc, u8 flags, int def, tune_cb cb)
{
    struct tunable *t = alloc(mod, name, desc, TT_BOOL, flags, cb);
    t->val = t->def = def; t->min = 0; t->max = 1;
    apply_bootarg(t);
    return t;
}
struct tunable *tune_str(const char *mod, const char *name, const char *desc, u8 flags, const char *def, tune_cb cb)
{
    struct tunable *t = alloc(mod, name, desc, TT_STR, flags, cb);
    strlcpy(t->sval, def, sizeof t->sval);
    apply_bootarg(t);
    return t;
}

struct tunable *tune_find(const char *full)
{
    for (int i = 0; i < ntab; i++) if (!strcmp(tab[i].name, full)) return &tab[i];
    return NULL;
}
int tune_count(void) { return ntab; }
struct tunable *tune_at(int i) { return i >= 0 && i < ntab ? &tab[i] : NULL; }

int tune_set(const char *full, const char *text, const char **msg)
{
    static const char *dummy; if (!msg) msg = &dummy;
    struct tunable *t = tune_find(full);
    i64 iv = 0;
    if (!t) { *msg = "no such setting"; return -1; }
    if (t->flags & TF_RO) { *msg = "read-only"; return -1; }
    if (parse_for(t, text, &iv, msg)) return -1;
    if (t->flags & TF_BOOT) {
        if (t->type == TT_STR) strlcpy(t->spend, text, sizeof t->spend); else t->pending = iv;
        t->has_pending = 1;
        tune_bootarg_add(t->name, text);     /* so `tune save` / the next boot picks it up */
        *msg = "takes effect at next boot";
        return 1;
    }
    if (t->cb && t->cb(t, iv, text)) { *msg = "rejected by module"; return -1; }
    if (t->type == TT_STR) strlcpy(t->sval, text, sizeof t->sval); else t->val = iv;
    tune_bootarg_add(t->name, text);
    return 0;
}

void tune_format(const struct tunable *t, char *buf, size_t n)
{
    if (t->type == TT_STR) strlcpy(buf, t->sval, n);
    else if (t->type == TT_BOOL) strlcpy(buf, t->val ? "on" : "off", n);
    else ksnprintf(buf, n, "%ld", t->val);
}

int tune_save_text(char *buf, size_t n)
{
    size_t len = 0; char v[64];
    buf[0] = 0;
    for (int i = 0; i < ntab; i++) {
        struct tunable *t = &tab[i];
        if (t->flags & TF_RO) continue;
        const char *val = NULL;
        if (t->has_pending) { if (t->type == TT_STR) val = t->spend; else { tune_format(&(struct tunable){ .type = t->type, .val = t->pending }, v, sizeof v); val = v; } }
        else {
            int differs = t->type == TT_STR ? 0 : t->val != t->def;
            if (!differs) continue;
            tune_format(t, v, sizeof v); val = v;
        }
        len += ksnprintf(buf + len, n - len, "%s=%s\n", t->name, val);
        if (len + 1 >= n) break;
    }
    return (int)len;
}
