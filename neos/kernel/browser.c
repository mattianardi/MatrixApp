#include "browser.h"
#include "net.h"
#include "agent.h"
#include "vfs.h"
#include "fs.h"
#include "varbus.h"
#include "shell.h"

#define PAGE_MAX (192 * 1024)
#define TX_MAX 49152
#define MAX_ITEMS 2600
#define MAX_LINKS 160
#define MAX_FIELDS 64
#define MAX_FORMS 12
#define HIST 16

static int atoi_(const char *s) { int v = 0; while (is_digit(*s)) v = v * 10 + (*s++ - '0'); return v; }

char br_urlbar[BR_URL];
int br_focus = -1;
static char cur_url[BR_URL], title[64], status[96];
static char *page; static size_t page_len;
static char *tx; static size_t tx_n;
static struct bitem items[MAX_ITEMS]; static int nitems, doc_h;
static char links[MAX_LINKS][BR_URL]; static int nlinks;
static struct bfield fields[MAX_FIELDS]; static int nfields;
static struct { char action[120]; } forms[MAX_FORMS]; static int nforms;
static char hist[HIST][BR_URL]; static int hn, hpos = -1;
static int layout_w = 700;

/* async load state: 0 idle, 1 loading, 2 done (the thread filled `page`) */
static volatile int load_state; static char load_url[BR_URL]; static int load_status, load_ok; static char load_err[96];
static volatile int thread_started;

/* ---------------------------------------------------------------- small helpers */
struct sb { char *b; size_t n, cap; };
static void sb_f(struct sb *s, const char *fmt, ...)
{
    if (s->n + 1 >= s->cap) return;
    va_list ap; va_start(ap, fmt); int w = kvsnprintf(s->b + s->n, s->cap - s->n, fmt, ap); va_end(ap);
    if (w > 0) s->n += (size_t)w < s->cap - s->n ? (size_t)w : s->cap - s->n - 1;
}
static void sb_esc(struct sb *s, const char *t)
{
    for (; *t && s->n + 8 < s->cap; t++) {
        const char *r = *t == '<' ? "&lt;" : *t == '>' ? "&gt;" : *t == '&' ? "&amp;" : *t == '"' ? "&quot;" : NULL;
        if (r) { size_t l = strlen(r); memcpy(s->b + s->n, r, l); s->n += l; } else s->b[s->n++] = *t;
    }
    s->b[s->n] = 0;
}
static void sb_urlenc(struct sb *s, const char *t)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *t && s->n + 4 < s->cap; t++) {
        if (is_alpha(*t) || is_digit(*t) || *t == '-' || *t == '_' || *t == '.' || *t == '/') s->b[s->n++] = *t;
        else if (*t == ' ') s->b[s->n++] = '+';
        else { s->b[s->n++] = '%'; s->b[s->n++] = hex[(u8)*t >> 4]; s->b[s->n++] = hex[*t & 15]; }
    }
    s->b[s->n] = 0;
}
static int hexv(int c) { return is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }
static int qget(const char *q, const char *key, char *out, size_t on)
{
    size_t kl = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1; size_t l = 0;
            while (*v && *v != '&' && l + 1 < on) {
                if (*v == '+') { out[l++] = ' '; v++; }
                else if (*v == '%' && hexv(v[1]) >= 0 && hexv(v[2]) >= 0) { out[l++] = (char)(hexv(v[1]) * 16 + hexv(v[2])); v += 3; }
                else out[l++] = *v++;
            }
            out[l] = 0; return 0;
        }
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
    out[0] = 0; return -1;
}

/* ---------------------------------------------------------------- URLs */
static void resolve(const char *base, const char *href, char *out, size_t on)
{
    if (strstr(href, "://")) { strlcpy(out, href, on); return; }
    if (href[0] == '#' || !href[0]) { strlcpy(out, base, on); return; }
    const char *se = strstr(base, "://");
    if (!se) { strlcpy(out, href, on); return; }
    char origin[BR_URL]; size_t ol = (size_t)(se + 3 - base);
    const char *pe = se + 3; while (*pe && *pe != '/' && *pe != '?') pe++;
    size_t hl = (size_t)(pe - base); if (hl >= sizeof origin) hl = sizeof origin - 1;
    memcpy(origin, base, hl); origin[hl] = 0; (void)ol;
    if (href[0] == '/') { ksnprintf(out, on, "%s%s", origin, href); return; }
    if (href[0] == '?') { ksnprintf(out, on, "%s%s", origin, "") ; char path[BR_URL]; strlcpy(path, pe, sizeof path); char *q = strchr(path, '?'); if (q) *q = 0; ksnprintf(out, on, "%s%s%s", origin, path, href); return; }
    char dir[BR_URL]; strlcpy(dir, pe, sizeof dir); char *q = strchr(dir, '?'); if (q) *q = 0;
    char *sl = strrchr(dir, '/'); if (sl) sl[1] = 0; else dir[0] = '/', dir[1] = 0;
    ksnprintf(out, on, "%s%s%s", origin, dir, href);
}
static int scheme_is(const char *u, const char *s) { return !strncmp(u, s, strlen(s)); }

/* ---------------------------------------------------------------- built-in pages (neos://) */
static void page_head(struct sb *s, const char *t)
{
    sb_f(s, "<html><head><title>%s</title></head><body><h2>%s</h2><p><a href=\"neos://home\">Home</a> | <a href=\"neos://system\">System</a> | <a href=\"neos://settings\">Settings</a> | <a href=\"neos://agents\">Agents</a> | <a href=\"neos://files?path=/\">Files</a> | <a href=\"neos://about\">About</a></p><hr>", t, t);
}

static void app_home(struct sb *s)
{
    page_head(s, "NEOS - web workspace");
    sb_f(s, "<p>Web-based apps of this operating system. Every page below is served by the kernel itself (neos://) or by a file in /www.</p><ul>"
            "<li><a href=\"neos://system\">System</a> - threads, memory, real-time health</li>"
            "<li><a href=\"neos://settings\">Settings</a> - every module setting, changeable live from a form</li>"
            "<li><a href=\"neos://agents\">Agents</a> - AI agents, approvals, emergency freeze</li>"
            "<li><a href=\"neos://files?path=/\">Files</a> - browse the file system (Linux and Windows C: view)</li>"
            "<li><a href=\"neos://about\">About NEOS</a></li></ul>"
            "<p>Type a URL in the address bar: <b>http://host/path</b> (no TLS yet), <b>file:///mnt/c/www/index.html</b> or <b>neos://www/page.html</b> for pages written by Linux or Windows programs into /www.</p>");
    struct inode *w = vfs_lookup("/www");
    if (w) {
        sb_f(s, "<h3>Apps in /www</h3><ul>");
        char name[64]; int type; u32 ino;
        for (int i = 0; !vfs_readdir("/www", i, name, sizeof name, &type, &ino); i++) { sb_f(s, "<li><a href=\"neos://www/"); sb_esc(s, name); sb_f(s, "\">"); sb_esc(s, name); sb_f(s, "</a></li>"); }
        sb_f(s, "</ul>");
    }
    sb_f(s, "</body></html>");
}

static void app_system(struct sb *s)
{
    page_head(s, "System");
    u64 up = time_ns() / 1000000000ull, mn, mx, avg, n; timer_jitter(&mn, &mx, &avg, &n);
    char ip[24] = "-"; bus_get("NEOS.net.ip", NEOS_VIEW_LINUX, ip, sizeof ip);
    sb_f(s, "<p>%s %s - up %lu s - cpu %d%% - %lu MiB free - timer %u Hz, jitter max %lu us - ip %s</p>", NEOS_NAME, NEOS_VERSION, up, ui_cpu_pct(), (u64)pmm_free_pages() / 256, timer_hz(), mx / 1000, ip);
    sb_f(s, "<h3>Threads</h3><table><tr><th>id<th>name<th>prio<th>state<th>cpu ticks<th>max wake latency</tr>");
    struct thread_info ti;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) if (!thread_info(i, &ti))
        sb_f(s, "<tr><td>%d<td>%s<td>%d<td>%s<td>%lu<td>%lu us</tr>", ti.id, ti.name, ti.prio, ti.state, ti.runticks, ti.lat_max_ns / 1000);
    sb_f(s, "</table><h3>Modules</h3><table><tr><th>module<th>version<th>description</tr>");
    for (int i = 0; i < module_count(); i++) { const char *nm, *v, *d; module_get(i, &nm, &v, &d); sb_f(s, "<tr><td>%s<td>%s<td>", nm, v); sb_esc(s, d); sb_f(s, "</tr>"); }
    sb_f(s, "</table></body></html>");
}

static void app_settings(struct sb *s, const char *q)
{
    char name[48], val[64], m[24];
    page_head(s, "Settings");
    if (!qget(q, "name", name, sizeof name) && !qget(q, "value", val, sizeof val)) {
        const char *msg = ""; int r = tune_set(name, val, &msg);
        sb_f(s, "<p><b>");
        if (r < 0) { sb_f(s, "error: "); sb_esc(s, name); sb_f(s, ": "); sb_esc(s, msg); }
        else { sb_esc(s, name); sb_f(s, " = "); sb_esc(s, val); sb_f(s, r ? " (applies at next boot)" : " (applied)"); }
        sb_f(s, "</b></p>");
    }
    if (qget(q, "m", m, sizeof m)) {
        sb_f(s, "<p>Pick a module. Changes apply live (or at the next boot when marked boot).</p><ul>");
        char seen[24][24]; int ns = 0;
        for (int i = 0; i < tune_count(); i++) {
            struct tunable *t = tune_at(i); int dup = 0;
            for (int k = 0; k < ns; k++) if (!strcmp(seen[k], t->module)) dup = 1;
            if (dup || ns >= 24) continue;
            strlcpy(seen[ns++], t->module, 24);
            sb_f(s, "<li><a href=\"neos://settings?m=%s\">%s</a></li>", t->module, t->module);
        }
        sb_f(s, "</ul></body></html>"); return;
    }
    sb_f(s, "<p>Module <b>%s</b> - <a href=\"neos://settings\">all modules</a></p>", m);
    for (int i = 0; i < tune_count(); i++) {
        struct tunable *t = tune_at(i);
        if (strcmp(t->module, m)) continue;
        char cur[64]; tune_format(t, cur, sizeof cur);
        sb_f(s, "<form action=\"neos://settings\"><input type=\"hidden\" name=\"m\" value=\"%s\"><input type=\"hidden\" name=\"name\" value=\"%s\"><b>%s</b> ", m, t->name, t->name);
        if (t->flags & TF_RO) { sb_f(s, "= %s (read-only)", cur); }
        else { sb_f(s, "<input name=\"value\" size=\"14\" value=\""); sb_esc(s, cur); sb_f(s, "\"> <input type=\"submit\" value=\"Set\">"); }
        sb_f(s, "<br>"); sb_esc(s, t->desc); sb_f(s, "</form><hr>");
    }
    sb_f(s, "</body></html>");
}

static void app_agents(struct sb *s, const char *q)
{
    char v[16];
    if (!qget(q, "approve", v, sizeof v)) approval_decide((int)(v[0] ? atoi_(v) : 0), 1);
    if (!qget(q, "deny", v, sizeof v)) approval_decide((int)(v[0] ? atoi_(v) : 0), 0);
    if (!qget(q, "freeze", v, sizeof v)) { if (v[0] == '1') agent_freeze(); else agent_thaw(); }
    page_head(s, "Agents");
    sb_f(s, "<p>Agents are kernel principals: every action goes through one gate with capabilities, approval policy and an audit chain. ");
    if (agent_is_frozen()) sb_f(s, "<b>ALL AGENTS ARE FROZEN.</b> <a href=\"neos://agents?freeze=0\">[Thaw]</a></p>");
    else sb_f(s, "<a href=\"neos://agents?freeze=1\">[Freeze all agents]</a></p>");
    sb_f(s, "<table><tr><th>agent<th>kind<th>calls<th>denied<th>state</tr>");
    for (int i = 0; i < AG_MAX; i++) { struct agent *a = agent_at(i); if (!a) continue; sb_f(s, "<tr><td>%s<td>%s<td>%lu<td>%lu<td>%s</tr>", a->name, a->kind == AK_NATIVE ? "native" : "remote", a->calls, a->denied, a->state == AS_ACTIVE ? "active" : "suspended"); }
    sb_f(s, "</table><h3>Pending approvals</h3>");
    int pend = approval_count_pending();
    if (!pend) sb_f(s, "<p>none</p>");
    for (int i = 0; i < pend; i++) {
        struct approval ap; if (approval_get(i, &ap)) break;
        struct agent *a = agent_at(ap.agent);
        sb_f(s, "<p>#%d %s wants <b>%s</b> ", ap.id, a ? a->name : "?", ap.tool); sb_esc(s, ap.args);
        sb_f(s, " <a href=\"neos://agents?approve=%d\">[Approve]</a> <a href=\"neos://agents?deny=%d\">[Deny]</a></p>", ap.id, ap.id);
    }
    sb_f(s, "<h3>Audit (newest first)</h3><pre>");
    struct audit_rec r;
    for (int i = 0; i < 8 && !audit_get(i, &r); i++) { char l[120]; ksnprintf(l, sizeof l, "#%lu %s %s rc=%d\n", r.seq, r.tool, r.args, r.rc); sb_esc(s, l); }
    sb_f(s, "</pre></body></html>");
}

static void app_files(struct sb *s, const char *q)
{
    char path[128];
    if (qget(q, "path", path, sizeof path) || !path[0]) strcpy(path, "/");
    page_head(s, "Files");
    sb_f(s, "<p>"); sb_esc(s, path); sb_f(s, " - Windows view: ");
    char w[160]; path_posix_to_win(path, w, sizeof w); sb_esc(s, w); sb_f(s, "</p>");
    struct inode *ino = vfs_lookup(path);
    if (ino && (ino->mode & S_IFMT) == S_IFREG) {
        sb_f(s, "<p><a href=\"neos://files?path="); char up[128]; strlcpy(up, path, sizeof up); char *sl = strrchr(up, '/'); if (sl == up) up[1] = 0; else if (sl) *sl = 0; sb_urlenc(s, up);
        sb_f(s, "\">[up]</a> %lu bytes</p><pre>", (u64)ino->size);
        size_t n = ino->size < 6000 ? ino->size : 6000; char *tmp = kmalloc(n + 1);
        if (tmp) { inode_read(ino, 0, tmp, n); for (size_t i = 0; i < n; i++) if ((u8)tmp[i] < 9 || ((u8)tmp[i] > 13 && (u8)tmp[i] < 32)) tmp[i] = '.'; tmp[n] = 0; sb_esc(s, tmp); kfree(tmp); }
        sb_f(s, "</pre></body></html>"); return;
    }
    if (strcmp(path, "/")) { char up[128]; strlcpy(up, path, sizeof up); char *sl = strrchr(up, '/'); if (sl == up) up[1] = 0; else if (sl) *sl = 0; sb_f(s, "<p><a href=\"neos://files?path="); sb_urlenc(s, up); sb_f(s, "\">[up]</a></p>"); }
    sb_f(s, "<ul>");
    char name[64]; int type; u32 ino2;
    for (int i = 0; !vfs_readdir(path, i, name, sizeof name, &type, &ino2); i++) {
        char full[200]; ksnprintf(full, sizeof full, "%s%s%s", path, strcmp(path, "/") ? "/" : "", name);
        sb_f(s, "<li><a href=\"neos://files?path="); sb_urlenc(s, full); sb_f(s, "\">"); sb_esc(s, name); sb_f(s, type ? "/" : ""); sb_f(s, "</a></li>");
    }
    sb_f(s, "</ul></body></html>");
}

static void app_about(struct sb *s)
{
    page_head(s, "About NEOS");
    sb_f(s, "<p><b>%s %s</b> - a real-time operating system by Mattia Nardi.</p><ul>"
            "<li>deterministic fixed-priority scheduler, tunable live</li>"
            "<li>kernel-level AI agents with capabilities, approvals and an audit chain</li>"
            "<li>runs Linux (ELF) and Windows (PE64) programs natively, cooperating through one variable bus</li>"
            "<li>Nio, the native language; AIH, natural-language help for every command</li>"
            "<li>NeoFS persistent storage; graphical workspace; this browser</li></ul>"
            "<p>Browser limits: HTTP only (no TLS), HTML subset, no JavaScript or CSS, forms are sent with GET.</p></body></html>", NEOS_NAME, NEOS_VERSION);
}

static void load_file(const char *path, struct sb *s)
{
    const u8 *d; size_t n;
    if (fs_read(path, &d, &n)) {
        page_head(s, "Not found"); sb_f(s, "<p>No such file: "); sb_esc(s, path); sb_f(s, "</p></body></html>"); return;
    }
    size_t l = strlen(path);
    int html = (l > 5 && !strcmp(path + l - 5, ".html")) || (l > 4 && !strcmp(path + l - 4, ".htm"));
    if (html) { size_t c = n < s->cap - 1 ? n : s->cap - 1; memcpy(s->b, d, c); s->n = c; s->b[c] = 0; return; }
    sb_f(s, "<html><body><pre>");
    size_t c = n < 20000 ? n : 20000; char *tmp = kmalloc(c + 1);
    if (tmp) { memcpy(tmp, d, c); for (size_t i = 0; i < c; i++) if ((u8)tmp[i] < 9 || ((u8)tmp[i] > 13 && (u8)tmp[i] < 32)) tmp[i] = '.'; tmp[c] = 0; sb_esc(s, tmp); kfree(tmp); }
    sb_f(s, "</pre></body></html>");
}

static void build_internal(const char *url, struct sb *s)
{
    const char *p = url + 7; char name[32]; size_t k = 0;
    while (*p && *p != '/' && *p != '?' && k < sizeof name - 1) name[k++] = *p++;
    name[k] = 0;
    const char *q = *p == '?' ? p + 1 : "";
    if (*p == '/' && !strcmp(name, "www")) { char f[160]; const char *rest = p + 1; char r2[120]; size_t i = 0; while (*rest && *rest != '?' && i < sizeof r2 - 1) r2[i++] = *rest++; r2[i] = 0; ksnprintf(f, sizeof f, "/www/%s", r2); load_file(f, s); return; }
    if (!strcmp(name, "home") || !name[0]) app_home(s);
    else if (!strcmp(name, "system")) app_system(s);
    else if (!strcmp(name, "settings")) app_settings(s, q);
    else if (!strcmp(name, "agents")) app_agents(s, q);
    else if (!strcmp(name, "files")) app_files(s, q);
    else if (!strcmp(name, "about")) app_about(s);
    else { page_head(s, "Unknown page"); sb_f(s, "<p>There is no neos://%s app.</p></body></html>", name); }
}

/* ---------------------------------------------------------------- loading */
static void load_thread(void *arg)
{
    (void)arg;
    for (;;) {
        while (load_state != 1) thread_sleep_ms(20);
        size_t len = 0; int st = 0; const char *err = "";
        int rc = http_get(load_url, page, PAGE_MAX - 1, &len, &st, &err);
        if (rc) { load_ok = 0; strlcpy(load_err, err, sizeof load_err); page_len = 0; }
        else { load_ok = 1; load_status = st; page_len = len; page[len] = 0; }
        load_state = 2;
    }
}

static void finish_page(const char *url);

static void set_error(const char *msg)
{
    struct sb s = { page, 0, PAGE_MAX };
    sb_f(&s, "<html><body><h2>Cannot open the page</h2><p>"); sb_esc(&s, msg); sb_f(&s, "</p><p><a href=\"neos://home\">neos://home</a></p></body></html>");
    page_len = s.n;
}

static void navigate(const char *url, int push)
{
    char u[BR_URL];
    strlcpy(u, url, sizeof u);
    if (!strstr(u, "://")) {
        if (u[0] == '/') { char t[BR_URL]; ksnprintf(t, sizeof t, "file://%s", u); strlcpy(u, t, sizeof u); }
        else { char t[BR_URL]; ksnprintf(t, sizeof t, "http://%s", u); strlcpy(u, t, sizeof u); }
    }
    if (push) {
        if (hpos + 1 < HIST) { hpos++; } else { for (int i = 1; i < HIST; i++) strlcpy(hist[i - 1], hist[i], BR_URL); }
        strlcpy(hist[hpos], u, BR_URL); hn = hpos + 1;
    }
    strlcpy(br_urlbar, u, sizeof br_urlbar);
    strlcpy(status, "", sizeof status);
    if (scheme_is(u, "neos://")) { struct sb s = { page, 0, PAGE_MAX }; build_internal(u, &s); page_len = s.n; finish_page(u); }
    else if (scheme_is(u, "file://")) { struct sb s = { page, 0, PAGE_MAX }; load_file(u + 7, &s); page_len = s.n; finish_page(u); }
    else if (scheme_is(u, "http://")) {
        if (load_state == 1) { strlcpy(status, "still loading the previous page", sizeof status); return; }
        strlcpy(load_url, u, sizeof load_url); strlcpy(status, "loading...", sizeof status); load_state = 1;
    } else if (scheme_is(u, "https://")) { set_error("https:// needs TLS, which NEOS does not have yet. Use http:// sites, neos:// apps, or files."); finish_page(u); }
    else { set_error("unsupported URL scheme"); finish_page(u); }
}

void br_go(const char *url, int typed)
{
    char u[BR_URL]; strlcpy(u, url, sizeof u);
    if (!typed && scheme_is(u, "neos://") && scheme_is(cur_url, "http")) { strlcpy(status, "blocked: web pages cannot open neos:// links", sizeof status); return; }
    br_focus = -1;
    navigate(u, 1);
}
void br_back(void) { if (hpos > 0) { hpos--; navigate(hist[hpos], 0); } }
void br_forward(void) { if (hpos + 1 < hn) { hpos++; navigate(hist[hpos], 0); } }
void br_reload(void) { if (cur_url[0]) { char u[BR_URL]; strlcpy(u, cur_url, sizeof u); navigate(u, 0); } }
int br_can_back(void) { return hpos > 0; }
int br_can_forward(void) { return hpos + 1 < hn; }
int br_loading(void) { return load_state == 1; }
const char *br_title(void) { return title; }
const char *br_status(void) { return status; }
int br_nitems(void) { return nitems; }
const struct bitem *br_item(int i) { return &items[i]; }
const struct bfield *br_field(int i) { return &fields[i]; }
int br_doc_height(void) { return doc_h; }

/* ---------------------------------------------------------------- HTML layout */
struct lay { int x, y, left, right, lineh, scale, bold, pre, ul, sp, tdx, tablecol, listdepth, skip_text, in_title, form; u32 col; };
static struct lay L;

static void newline(void) { L.y += L.lineh ? L.lineh + 2 : 16 * L.scale + 2; L.x = L.left; L.lineh = 0; L.sp = 0; L.tablecol = 0; }
static void block_break(int gap) { if (L.x > L.left || L.lineh) newline(); L.y += gap; }

static struct bitem *new_item(int kind, int w, int h)
{
    if (nitems >= MAX_ITEMS) return NULL;
    struct bitem *it = &items[nitems++];
    memset(it, 0, sizeof *it);
    it->kind = (u8)kind; it->w = (short)w; it->h = (short)h; it->scale = (u8)L.scale; it->col = L.col; it->link = -1; it->field = -1; it->bold = (u8)L.bold;
    return it;
}
static int cur_link = -1;

static void place(int kind, int w, int h)
{
    if (L.sp && L.x > L.left) L.x += 8 * L.scale;
    L.sp = 0;
    if (L.x + w > L.right && L.x > L.left) newline();
}

static void put_word(const char *w, int len)
{
    while (len > 0) {
        int maxc = (L.right - L.left) / (8 * L.scale); if (maxc < 1) maxc = 1;
        int chunk = len > maxc ? maxc : len;
        place(IT_TEXT, chunk * 8 * L.scale, 16 * L.scale);
        struct bitem *it = new_item(IT_TEXT, chunk * 8 * L.scale, 16 * L.scale);
        if (!it) return;
        if (tx_n + (size_t)chunk + 1 > TX_MAX) return;
        memcpy(tx + tx_n, w, (size_t)chunk); tx[tx_n + (size_t)chunk] = 0; it->text = tx + tx_n; it->len = (u8)chunk; tx_n += (size_t)chunk + 1;
        it->x = (short)L.x; it->y = (short)L.y; it->link = (short)cur_link; it->ul = cur_link >= 0;
        if (cur_link >= 0) it->col = 0x6CB6FF;
        L.x += chunk * 8 * L.scale; if (16 * L.scale > L.lineh) L.lineh = 16 * L.scale;
        w += chunk; len -= chunk;
        if (len > 0) newline();
    }
}

static int get_attr(const char *tag, const char *name, char *out, size_t on)
{
    size_t nl = strlen(name);
    for (const char *p = tag; *p; p++) {
        if ((p == tag || p[-1] == ' ') && !strncasecmp(p, name, nl) && p[nl] == '=') {
            p += nl + 1; char q = *p == '"' || *p == '\'' ? *p++ : 0; size_t l = 0;
            while (*p && (q ? *p != q : (*p != ' ' && *p != '>')) && l + 1 < on) out[l++] = *p++;
            out[l] = 0; return 0;
        }
    }
    return -1;
}
static int has_flag(const char *tag, const char *name) { for (const char *p = tag; *p; p++) if ((p == tag || p[-1] == ' ') && !strncasecmp(p, name, strlen(name)) && (p[strlen(name)] == 0 || p[strlen(name)] == ' ' || p[strlen(name)] == '=')) return 1; return 0; }

static int add_field(int type, const char *name, const char *value)
{
    if (nfields >= MAX_FIELDS) return -1;
    struct bfield *f = &fields[nfields]; memset(f, 0, sizeof *f);
    f->type = (u8)type; f->form = (short)L.form; strlcpy(f->name, name, sizeof f->name); strlcpy(f->value, value, sizeof f->value);
    return nfields++;
}

static void do_tag(char *tag, int closing)
{
    char name[12]; size_t k = 0;
    while (tag[k] && tag[k] != ' ' && k < sizeof name - 1) { name[k] = (char)to_lower(tag[k]); k++; } name[k] = 0;
    const char *attrs = tag + k;
    #define IS(n) (!strcmp(name, n))
    if (IS("h1") || IS("h2") || IS("h3") || IS("h4") || IS("h5") || IS("h6")) {
        block_break(closing ? 6 : 8);
        if (!closing) { L.scale = IS("h1") ? 2 : 1; L.bold = 1; L.col = IS("h1") ? 0xFFD479 : 0xA5D6FF; } else { L.scale = 1; L.bold = 0; L.col = 0xE6EDF7; }
    } else if (IS("p") || IS("div") || IS("center") || IS("blockquote")) block_break(closing ? 4 : 4);
    else if (IS("br")) newline();
    else if (IS("hr")) { block_break(4); struct bitem *it = new_item(IT_RULE, L.right - L.left, 2); if (it) { it->x = (short)L.left; it->y = (short)L.y; } L.y += 8; }
    else if (IS("ul") || IS("ol")) { block_break(2); if (closing) { L.left -= 24; if (L.left < 8) L.left = 8; } else L.left += 24; L.x = L.left; }
    else if (IS("li")) { if (!closing) { block_break(0); struct bitem *it = new_item(IT_BULLET, 8, 16); if (it) { it->x = (short)(L.left - 14); it->y = (short)L.y; } } }
    else if (IS("b") || IS("strong") || IS("th")) { L.bold = !closing; L.col = closing ? 0xE6EDF7 : 0xFFFFFF; if (IS("th") && !closing) goto cell; }
    else if (IS("i") || IS("em") || IS("u") || IS("font") || IS("span") || IS("small") || IS("big")) { }
    else if (IS("code") || IS("tt")) L.col = closing ? 0xE6EDF7 : 0x9BE9A8;
    else if (IS("pre")) { block_break(4); L.pre = !closing; if (closing) block_break(4); }
    else if (IS("a")) {
        if (closing) cur_link = -1;
        else { char h[BR_URL]; if (!get_attr(attrs, "href", h, sizeof h) && nlinks < MAX_LINKS) { resolve(cur_url, h, links[nlinks], BR_URL); cur_link = nlinks++; } }
    }
    else if (IS("table")) { block_break(4); L.tablecol = 0; }
    else if (IS("tr")) { if (L.x > L.left) newline(); L.tablecol = 0; }
    else if (IS("td")) { cell: if (!closing) { int cx = L.left + L.tablecol * 150; if (L.x < cx) L.x = cx; L.tablecol++; L.sp = 0; } }
    else if (IS("form")) {
        if (!closing) { block_break(4); if (nforms < MAX_FORMS) { char a[120] = ""; get_attr(attrs, "action", a, sizeof a); resolve(cur_url, a, forms[nforms].action, sizeof forms[nforms].action); L.form = nforms++; } }
        else { L.form = -1; block_break(4); }
    }
    else if (IS("input") || IS("textarea") || IS("select")) {
        char type[12] = "text", nm[24] = "", val[64] = "", sz[6] = "";
        get_attr(attrs, "type", type, sizeof type); get_attr(attrs, "name", nm, sizeof nm); get_attr(attrs, "value", val, sizeof val); get_attr(attrs, "size", sz, sizeof sz);
        if (!strcasecmp(type, "hidden")) { add_field(FT_HIDDEN, nm, val); return; }
        int ft = !strcasecmp(type, "submit") || !strcasecmp(type, "button") ? FT_SUBMIT : !strcasecmp(type, "checkbox") ? FT_CHECK : !strcasecmp(type, "password") ? FT_PASS : FT_TEXT;
        int fi = add_field(ft, nm, ft == FT_SUBMIT && !val[0] ? "Submit" : val); if (fi < 0) return;
        if (ft == FT_CHECK) fields[fi].checked = has_flag(attrs, "checked");
        int w, h = 20;
        if (ft == FT_SUBMIT) w = (int)strlen(fields[fi].value) * 8 + 16;
        else if (ft == FT_CHECK) { w = 16; h = 16; }
        else { int n = sz[0] ? atoi_(sz) : 16; if (n < 4) n = 4; if (n > 60) n = 60; w = n * 8 + 8; }
        place(IT_INPUT, w, h);
        struct bitem *it = new_item(ft == FT_SUBMIT ? IT_BTN : IT_INPUT, w, h);
        if (it) { it->x = (short)L.x; it->y = (short)L.y; it->field = (short)fi; }
        L.x += w + 6; if (h > L.lineh) L.lineh = h;
    }
    else if (IS("img")) { char alt[40] = "image"; get_attr(attrs, "alt", alt, sizeof alt); char t[48]; ksnprintf(t, sizeof t, "[%s]", alt); put_word(t, (int)strlen(t)); }
    #undef IS
}

static void layout_doc(void)
{
    nitems = nlinks = nfields = nforms = 0; tx_n = 0; title[0] = 0; cur_link = -1;
    memset(&L, 0, sizeof L);
    L.left = 8; L.right = layout_w - 8; L.x = L.left; L.y = 6; L.scale = 1; L.col = 0xE6EDF7; L.form = -1;
    size_t i = 0;
    while (i < page_len) {
        if (page[i] == '<') {
            size_t j = i + 1; while (j < page_len && page[j] != '>') j++;
            if (j >= page_len) break;
            if (page[i + 1] == '!') { if (!strncmp(page + i, "<!--", 4)) { char *e = NULL; for (size_t k = i + 4; k + 2 < page_len; k++) if (page[k] == '-' && page[k + 1] == '-' && page[k + 2] == '>') { e = page + k; break; } if (e) { i = (size_t)(e - page) + 3; continue; } } i = j + 1; continue; }
            char tag[200]; size_t tl = j - i - 1; if (tl >= sizeof tag) tl = sizeof tag - 1;
            int closing = page[i + 1] == '/';
            memcpy(tag, page + i + 1 + closing, tl - (size_t)closing); tag[tl - (size_t)closing] = 0;
            for (char *c = tag; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
            size_t tn = strlen(tag); if (tn && tag[tn - 1] == '/') tag[tn - 1] = 0;
            i = j + 1;
            if (!strncasecmp(tag, "script", 6) || !strncasecmp(tag, "style", 5)) {
                if (!closing) { const char *endn = !strncasecmp(tag, "script", 6) ? "</script" : "</style"; size_t en = strlen(endn);
                    while (i < page_len && strncasecmp(page + i, endn, en)) i++;
                    while (i < page_len && page[i] != '>') i++; i++; }
                continue;
            }
            if (!strncasecmp(tag, "title", 5)) { if (!closing) { size_t k = i, t = 0; while (k < page_len && page[k] != '<' && t + 1 < sizeof title) title[t++] = page[k++]; title[t] = 0; i = k; } continue; }
            if (!strncasecmp(tag, "button", 6) && !closing) {
                size_t k = i, t = 0; char lab[24] = ""; while (k < page_len && page[k] != '<' && t + 1 < sizeof lab) lab[t++] = page[k++]; lab[t] = 0; i = k;
                char nm[24] = "", val[40]; get_attr(tag + 6, "name", nm, sizeof nm); strlcpy(val, lab[0] ? lab : "Submit", sizeof val);
                char fake[120]; ksnprintf(fake, sizeof fake, "input type=submit name=%s value=\"%s\"", nm, val); do_tag(fake, 0); continue;
            }
            do_tag(tag, closing);
            continue;
        }
        /* text run */
        char word[96]; int wl = 0;
        while (i < page_len && page[i] != '<') {
            unsigned char c = (unsigned char)page[i];
            if (c == '&') {
                size_t e = i + 1; while (e < page_len && e - i < 8 && page[e] != ';' && page[e] != ' ') e++;
                char ent[10]; size_t el = e - i - 1; if (el < sizeof ent) { memcpy(ent, page + i + 1, el); ent[el] = 0; }
                int rep = -1;
                if (page[e] == ';') { if (!strcmp(ent, "lt")) rep = '<'; else if (!strcmp(ent, "gt")) rep = '>'; else if (!strcmp(ent, "amp")) rep = '&'; else if (!strcmp(ent, "quot")) rep = '"'; else if (!strcmp(ent, "nbsp")) rep = ' '; else if (!strcmp(ent, "apos")) rep = '\''; else if (ent[0] == '#') rep = atoi_(ent + 1); }
                if (rep >= 0) { c = (unsigned char)(rep > 255 ? '?' : rep); i = e + 1; goto have; }
            }
            i++;
        have:
            if (L.pre) {
                if (c == '\n') { if (wl) { put_word(word, wl); wl = 0; } newline(); continue; }
                if (c == ' ' || c == '\t') { if (wl) { put_word(word, wl); wl = 0; } L.x += 8 * (c == '\t' ? 4 : 1); continue; }
            } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { if (wl) { put_word(word, wl); wl = 0; } L.sp = 1; continue; }
            if (wl < (int)sizeof word - 1) word[wl++] = (char)c;
        }
        if (wl) put_word(word, wl);
    }
    if (L.x > L.left) newline();
    doc_h = L.y + 10;
}

static void finish_page(const char *url)
{
    strlcpy(cur_url, url, sizeof cur_url);
    layout_doc();
    if (!status[0]) strlcpy(status, "done", sizeof status);
    br_focus = -1;
}

void br_layout(int width) { if (width != layout_w) { layout_w = width; if (page) layout_doc(); } }

int br_tick(void)
{
    if (load_state != 2) return 0;
    if (!load_ok) { set_error(load_err); finish_page(load_url); }
    else { ksnprintf(status, sizeof status, "HTTP %d, %lu bytes", load_status, (u64)page_len); finish_page(load_url); }
    load_state = 0;
    return 1;
}

/* ---------------------------------------------------------------- forms, keys */
void br_follow(int link) { if (link >= 0 && link < nlinks) { char u[BR_URL]; strlcpy(u, links[link], sizeof u); br_go(u, 0); } }

void br_submit(int fi)
{
    if (fi < 0 || fi >= nfields) return;
    int form = fields[fi].form; if (form < 0 || form >= nforms) return;
    char url[BR_URL * 2]; struct sb s = { url, 0, sizeof url };
    sb_f(&s, "%s%c", forms[form].action, strchr(forms[form].action, '?') ? '&' : '?');
    int first = 1;
    for (int i = 0; i < nfields; i++) {
        struct bfield *f = &fields[i];
        if (f->form != form || !f->name[0]) continue;
        if (f->type == FT_SUBMIT && i != fi) continue;
        if (f->type == FT_CHECK && !f->checked) continue;
        if (!first) sb_f(&s, "&"); first = 0;
        sb_urlenc(&s, f->name); sb_f(&s, "="); sb_urlenc(&s, f->value);
    }
    char u2[BR_URL]; strlcpy(u2, url, sizeof u2);
    br_go(u2, 0);
}
void br_toggle(int fi) { if (fi >= 0 && fi < nfields && fields[fi].type == FT_CHECK) fields[fi].checked ^= 1; }

void br_key(int c)
{
    if (br_focus == -2) {
        size_t l = strlen(br_urlbar);
        if (c == '\n') { char u[BR_URL]; strlcpy(u, br_urlbar, sizeof u); br_go(u, 1 ? 1 : 0); return; }
        if (c == '\b') { if (l) br_urlbar[l - 1] = 0; }
        else if (c >= 32 && c < 127 && l + 1 < sizeof br_urlbar) { br_urlbar[l] = (char)c; br_urlbar[l + 1] = 0; }
        return;
    }
    if (br_focus >= 0 && br_focus < nfields) {
        struct bfield *f = &fields[br_focus]; size_t l = strlen(f->value);
        if (c == '\n') { for (int i = br_focus; i < nfields; i++) if (fields[i].form == f->form && fields[i].type == FT_SUBMIT) { br_submit(i); return; } br_submit(br_focus); return; }
        if (c == '\b') { if (l) f->value[l - 1] = 0; }
        else if (c == '\t') { for (int k = 1; k <= nfields; k++) { int n = (br_focus + k) % nfields; if (fields[n].type == FT_TEXT || fields[n].type == FT_PASS) { br_focus = n; break; } } }
        else if (c >= 32 && c < 127 && l + 1 < sizeof f->value && f->type != FT_SUBMIT) { f->value[l] = (char)c; f->value[l + 1] = 0; }
    }
}

void br_init(void)
{
    u64 pa = pmm_alloc(PAGE_MAX / 4096 + 1), pt = pmm_alloc(TX_MAX / 4096 + 1);
    if (!pa || !pt) return;
    page = P2V(pa); tx = P2V(pt);
    if (!thread_started) { thread_started = 1; thread_create("browser-net", 6, load_thread, NULL); }
    module_register("browser", "1.0", "web browser engine: http / file / neos:// pages, HTML subset, forms");
    navigate("neos://home", 1);
}
