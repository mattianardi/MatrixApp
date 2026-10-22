#include "fs.h"
#include "net.h"
#include "shell.h"
#include "vfs.h"


/*
 * Package manager. Repository layout (see tools/mkrepo.py):
 *   <repo>/index.txt : "name version size sha256 file description..."
 *   <repo>/<file>    : NPK container  "NPK1\n" { "F <relpath> <size>\n" <bytes> "\n" } "END\n"
 * Files are installed under /opt/<name>/ and recorded in /var/pkg/<name>.
 * Integrity: package size+SHA-256 come from the index; the index itself can be pinned with
 * pkg.index_sha256. Transport is plain HTTP until TLS exists, so it is refused unless
 * pkg.allow_insecure is on.
 */
#define MAXP 256
struct pent { char name[24], ver[12], sha[65], file[48], desc[64]; size_t size; char repo[96]; u8 trusted; };
static struct pent idx[MAXP];
static int nidx;
static struct tunable *t_repo, *t_insecure, *t_pin, *t_maxkib, *t_syspaths;

void pkg_init(void)
{
    module_register("pkg", "0.1", "package manager: HTTP repository, SHA-256 verified NPK packages");
    t_repo     = tune_str ("pkg", "repo", "repositories, space separated: file:///dir (the offline cache shipped with the OS) and/or http://host:port", TF_LIVE, "file:///var/cache/pkg http://10.0.2.2:8080", NULL);
    t_syspaths = tune_bool("pkg", "allow_system_paths", "let network packages install under /lib, /lib64, /usr/lib (packages from the built-in cache always may)", TF_LIVE, 0, NULL);
    t_insecure = tune_bool("pkg", "allow_insecure", "allow plain-HTTP repositories (no TLS yet)", TF_LIVE, 0, NULL);
    t_pin      = tune_str ("pkg", "index_sha256", "if set, `pkg update` rejects an index with a different SHA-256", TF_LIVE, "", NULL);
    t_maxkib   = tune_int ("pkg", "max_kib", "largest package/index accepted (KiB)", TF_LIVE, 32768, 16, 262144, NULL);
}

static int repo_is_file(const char *r) { return !strncmp(r, "file://", 7); }

static int check_transport(const char *repo, const char **err)
{
    if (repo_is_file(repo)) return 0;
    if (!strncmp(repo, "https://", 8)) { *err = "https:// repositories need TLS, which NEOS does not have yet"; return -1; }
    if (!t_insecure->val) { *err = "plain-HTTP repository refused: `tune set pkg.allow_insecure on` to accept the risk (pin the index with pkg.index_sha256)"; return -1; }
    return 0;
}

/* *owned = 1: the caller must kfree(*buf); 0: it points into the filesystem and must not be freed or modified */
static int fetch(const char *repo, const char *name, void **buf, size_t *len, int *owned, const char **err)
{
    char url[256];
    if (repo_is_file(repo)) {
        ksnprintf(url, sizeof url, "%s/%s", repo + 7, name);
        const u8 *d; size_t n;
        if (fs_read(url, &d, &n)) { *err = "not in the offline cache"; return -1; }
        *buf = (void *)d; *len = n; *owned = 0;
        return 0;
    }
    size_t cap = (size_t)t_maxkib->val * 1024;
    ksnprintf(url, sizeof url, "%s/%s", repo, name);
    *buf = kmalloc(cap + 1);
    if (!*buf) { *err = "out of memory"; return -1; }
    int st = 0;
    if (http_get(url, *buf, cap, len, &st, err)) { kfree(*buf); return -1; }
    if (st != 200) { static char m[32]; ksnprintf(m, sizeof m, "server answered HTTP %d", st); *err = m; kfree(*buf); return -1; }
    *owned = 1;
    return 0;
}

static int installed(const char *name)
{
    char p[64]; const u8 *d; size_t n;
    ksnprintf(p, sizeof p, "/var/pkg/%s", name);
    return fs_read(p, &d, &n) == 0;
}

static void parse_index(char *s, const char *repo, int trusted)
{
    for (char *l = s, *e; *l && nidx < MAXP; l = e) {
        e = strchr(l, '\n'); if (e) *e++ = 0; else e = l + strlen(l);
        if (*l == '#' || !*l) continue;
        struct pent *p = &idx[nidx]; char sz[16] = "";
        char *f[5]; int k = 0;
        memset(p, 0, sizeof *p);
        for (char *q = l; *q && k < 5;) {
            while (*q == ' ') q++;
            if (!*q) break;
            f[k++] = q; while (*q && *q != ' ') q++;
            if (*q) *q++ = 0;
            if (k == 5) { strlcpy(p->desc, q, sizeof p->desc); break; }
        }
        if (k < 5) continue;
        int dup = 0;
        for (int i = 0; i < nidx; i++) if (!strcmp(idx[i].name, f[0])) dup = 1;      /* the first repository that has a name wins */
        if (dup) continue;
        strlcpy(p->name, f[0], sizeof p->name); strlcpy(p->ver, f[1], sizeof p->ver);
        strlcpy(sz, f[2], sizeof sz); strlcpy(p->sha, f[3], sizeof p->sha); strlcpy(p->file, f[4], sizeof p->file);
        strlcpy(p->repo, repo, sizeof p->repo); p->trusted = (u8)trusted;
        i64 v; if (parse_i64(sz, &v) || v < 0) continue;
        p->size = (size_t)v;
        nidx++;
    }
}

int pkg_update(const char **err)
{
    char repos[160]; strlcpy(repos, t_repo->sval, sizeof repos);
    nidx = 0; int okc = 0; const char *last = "no repository configured";
    for (char *r = repos; *r;) {
        while (*r == ' ') r++;
        char *e = r; while (*e && *e != ' ') e++;
        int more = *e; *e = 0;
        if (*r) {
            const char *er = "";
            void *b; size_t n; int owned = 0;
            if (check_transport(r, &er) || fetch(r, "index.txt", &b, &n, &owned, &er)) { kprintf("  %s: %s\n", r, er); last = er; }
            else {
                char h[65]; sha256_hex(b, n, h);
                if (!repo_is_file(r) && t_pin->sval[0] && strcasecmp(h, t_pin->sval)) { kprintf("  %s: index SHA-256 does not match pkg.index_sha256\n", r); last = "index SHA-256 does not match pkg.index_sha256"; if (owned) kfree(b); }
                else {
                    char *copy = kmalloc(n + 1);
                    if (copy) { memcpy(copy, b, n); copy[n] = 0; int before = nidx; parse_index(copy, r, repo_is_file(r)); kfree(copy); kprintf("  %s: %d packages%s\n", r, nidx - before, repo_is_file(r) ? "" : " (index sha256 pinned or not: see pkg.index_sha256)"); okc++; }
                    if (owned) kfree(b);
                }
            }
        }
        if (!more) break;
        r = e + 1;
    }
    if (!okc) { *err = last; return -1; }
    kprintf("index: %d packages\n", nidx);
    return 0;
}

int pkg_table(struct table *t)
{
    tb_init(t, "package", "version", "size", "state", "source", "description", NULL);
    for (int i = 0; i < nidx; i++) {
        if (tb_newrow(t) < 0) break;
        tb_set(t, 0, "%s", idx[i].name); tb_set(t, 1, "%s", idx[i].ver); tb_set(t, 2, "%lu", (u64)idx[i].size);
        tb_set(t, 3, "%s", installed(idx[i].name) ? "installed" : "-"); tb_set(t, 4, "%s", repo_is_file(idx[i].repo) ? "cache" : "net"); tb_set(t, 5, "%s", idx[i].desc);
    }
    return nidx;
}

int pkg_info(const char *name)
{
    for (int i = 0; i < nidx; i++) if (!strcmp(idx[i].name, name)) {
        kprintf("%s %s  %lu bytes\n  file   %s\n  sha256 %s\n  %s\n", idx[i].name, idx[i].ver, (u64)idx[i].size, idx[i].file, idx[i].sha, idx[i].desc);
        return 0;
    }
    return -1;
}

static int has_dotdot(const char *s)
{
    for (const char *p = s; *p; p++)
        if (p[0] == '.' && p[1] == '.' && (p == s || p[-1] == '/') && (p[2] == '/' || !p[2])) return 1;
    return 0;
}

/* walk an NPK; if `write`, install under /opt/<name>. Returns file count or -1. */
static int npk_walk(const char *root, const u8 *d, size_t n, int write, int sysok, const char **err)
{
    if (n < 5 || memcmp(d, "NPK1\n", 5)) { *err = "not an NPK package"; return -1; }
    size_t i = 5; int files = 0;
    while (i < n) {
        if (n - i >= 4 && !memcmp(d + i, "END\n", 4)) return files;
        if (d[i] != 'F' || d[i + 1] != ' ') { *err = "corrupt package"; return -1; }
        char rel[200]; size_t k = 0; i += 2;
        while (i < n && d[i] != ' ' && k < sizeof rel - 1) rel[k++] = (char)d[i++];
        rel[k] = 0;
        if (i >= n || d[i] != ' ') { *err = "corrupt package"; return -1; }
        i++;
        u64 sz = 0; while (i < n && is_digit(d[i])) sz = sz * 10 + (d[i++] - '0');
        if (i >= n || d[i] != '\n' || sz > n || i + 1 + sz + 1 > n) { *err = "corrupt package"; return -1; }
        i++;
        int sysabs = rel[0] == '/' && sysok && (!strncmp(rel, "/lib/", 5) || !strncmp(rel, "/lib64/", 7) || !strncmp(rel, "/usr/lib/", 9) || !strncmp(rel, "/usr/bin/", 9) || !strncmp(rel, "/etc/ssl/", 9));
        if (!rel[0] || (rel[0] == '/' && !sysabs) || has_dotdot(rel)) { *err = rel[0] == '/' ? "package wants system paths (pkg.allow_system_paths is off)" : "package contains an unsafe path"; return -1; }
        if (write) {
            char path[256]; if (sysabs) strlcpy(path, rel, sizeof path); else ksnprintf(path, sizeof path, "%s/%s", root, rel);
            if (fs_write(path, d + i, (size_t)sz)) { *err = "filesystem full"; return -1; }
            if (strstr(path, "/bin/") || strstr(path, ".so") || !strncmp(path, "/bin/", 5)) { struct inode *ni = vfs_lookup(path); if (ni) ni->mode |= 0111; }
        }
        i += (size_t)sz + 1;
        files++;
    }
    *err = "package has no END marker";
    return -1;
}

/* /opt/bin gets a symlink for every program a package ships in bin/ (PATH contains /opt/bin) */
static void link_bins(const char *name)
{
    char root[64]; ksnprintf(root, sizeof root, "/opt/%s/bin", name);
    char nm[64]; int type; u32 in;
    vfs_mkdir("/opt/bin", 0755);
    for (int i = 0; !vfs_readdir(root, i, nm, sizeof nm, &type, &in); i++) {
        char link[96], target[200];
        ksnprintf(link, sizeof link, "/opt/bin/%s", nm); ksnprintf(target, sizeof target, "%s/%s", root, nm);
        vfs_unlink(link); vfs_symlink(target, link);
    }
}

static void unlink_bins(const char *name)
{
    char prefix[80]; ksnprintf(prefix, sizeof prefix, "/opt/%s/", name);
    char nm[64]; int type; u32 in;
    for (int i = 0; !vfs_readdir("/opt/bin", i, nm, sizeof nm, &type, &in); ) {
        char link[96]; ksnprintf(link, sizeof link, "/opt/bin/%s", nm);
        struct inode *l = vfs_lookup(link);
        if (l && (l->mode & S_IFMT) == S_IFLNK && !strncmp(l->link, prefix, strlen(prefix))) vfs_unlink(link); else i++;
    }
}

int pkg_record(const char *name, const char *ver, const char *origin)
{
    char rec[96], body[160];
    ksnprintf(rec, sizeof rec, "/var/pkg/%s", name);
    int l = ksnprintf(body, sizeof body, "%s\n%s\n", ver, origin);
    return fs_write(rec, body, (size_t)l);
}

/* validate the whole package first, then replace /opt/<name>; returns file count or -1 */
static int install_blob(const char *name, const char *ver, const u8 *d, size_t n, int sysok, const char *origin, const char **err)
{
    char root[64];
    ksnprintf(root, sizeof root, "/opt/%s", name);
    int files = npk_walk(root, d, n, 0, sysok, err);
    if (files < 0) return -1;
    fs_remove_tree(root);
    npk_walk(root, d, n, 1, sysok, err);
    link_bins(name);
    pkg_record(name, ver, origin);
    return files;
}
int pkg_install_blob(const char *name, const char *ver, const u8 *d, size_t n, const char **err) { return install_blob(name, ver, d, n, (int)t_syspaths->val, "local", err); }

/* pack /src-style directory `dir` (absolute, normalized) into /var/dist/<name>-<ver>.npk */
int pkg_build(const char *name, const char *ver, const char *dir, char *outpath, size_t on, char sha[65], size_t *size, const char **err)
{
    size_t dl = strlen(dir), cap = 4096, len = 0;
    u8 *buf = kmalloc(cap);
    if (!buf) { *err = "out of memory"; return -1; }
    #define NEED(k) do { if (len + (k) > cap) { size_t nc = cap; while (nc < len + (k)) nc *= 2; u8 *nb = kmalloc(nc); \
        if (!nb) { kfree(buf); *err = "out of memory"; return -1; } memcpy(nb, buf, len); kfree(buf); buf = nb; cap = nc; } } while (0)
    NEED(5); memcpy(buf, "NPK1\n", 5); len = 5;
    int files = 0; char p[128]; size_t sz;
    for (int i = 0; !fs_info(i, p, sizeof p, &sz); i++) {
        if (strncmp(p, dir, dl) || p[dl] != '/') continue;
        const u8 *d; size_t n;
        if (fs_read(p, &d, &n)) continue;
        char hdr[96]; int hl = ksnprintf(hdr, sizeof hdr, "F %s %lu\n", p + dl + 1, (u64)n);
        NEED((size_t)hl + n + 1);
        memcpy(buf + len, hdr, hl); len += hl; memcpy(buf + len, d, n); len += n; buf[len++] = '\n';
        files++;
    }
    #undef NEED
    if (!files) { kfree(buf); *err = "directory has no files"; return -1; }
    if (len + 4 > cap) { u8 *nb = kmalloc(len + 4); if (!nb) { kfree(buf); *err = "out of memory"; return -1; } memcpy(nb, buf, len); kfree(buf); buf = nb; }
    memcpy(buf + len, "END\n", 4); len += 4;
    ksnprintf(outpath, on, "/var/dist/%s-%s.npk", name, ver);
    sha256_hex(buf, len, sha);
    int r = fs_write(outpath, buf, len);
    kfree(buf);
    if (r) { *err = "filesystem full"; return -1; }
    *size = len;
    return files;
}

static int npk_find(const u8 *d, size_t n, const char *want, const u8 **out, size_t *olen);
static void kv(const char *text, size_t n, const char *key, char *out, size_t on);
static int install_depth;

int pkg_install(const char *name, const char **err)
{
    struct pent *p = NULL;
    if (!nidx) { const char *ue = ""; if (pkg_update(&ue)) { *err = ue; return -1; } }          /* first use: read the repositories */
    for (int i = 0; i < nidx; i++) if (!strcmp(idx[i].name, name)) p = &idx[i];
    if (!p) { *err = "unknown package (run `pkg update`, then `pkg list`)"; return -1; }
    if (check_transport(p->repo, err)) return -1;
    void *b; size_t n; int owned = 0;
    kprintf("fetching %s %s (%lu bytes) from %s...\n", p->name, p->ver, (u64)p->size, p->repo);
    if (fetch(p->repo, p->file, &b, &n, &owned, err)) return -1;
    char h[65]; sha256_hex(b, n, h);
    if (n != p->size) { *err = "size differs from the index"; if (owned) kfree(b); return -1; }
    if (strcasecmp(h, p->sha)) { *err = "SHA-256 mismatch: package corrupted or tampered"; if (owned) kfree(b); return -1; }
    const u8 *pi; size_t pl;                                                /* dependencies come first (".pkginfo": depends=a,b) */
    if (!npk_find(b, n, ".pkginfo", &pi, &pl) && install_depth < 4) {
        char deps[96]; kv((const char *)pi, pl, "depends", deps, sizeof deps);
        for (char *d = deps; *d;) {
            char *e = d; while (*e && *e != ',') e++;
            int more = *e; *e = 0;
            if (*d && !installed(d)) { install_depth++; char nm[24]; strlcpy(nm, d, sizeof nm); int r = pkg_install(nm, err); install_depth--; if (r) { if (owned) kfree(b); return -1; } }
            if (!more) break;
            d = e + 1;
        }
    }
    char origin[100]; ksnprintf(origin, sizeof origin, "npk %s", p->repo);
    int files = install_blob(p->name, p->ver, b, n, p->trusted || (int)t_syspaths->val, origin, err);
    if (owned) kfree(b);
    if (files < 0) return -1;
    kprintf("installed %s %s: %d files in /opt/%s (sha256 verified)\n", p->name, p->ver, files, p->name);
    return 0;
}

int pkg_remove(const char *name, const char **err)
{
    char root[64], rec[64];
    ksnprintf(root, sizeof root, "/opt/%s", name); ksnprintf(rec, sizeof rec, "/var/pkg/%s", name);
    if (!installed(name)) { *err = "not installed"; return -1; }
    unlink_bins(name);
    int n = fs_remove_tree(root);
    fs_remove(rec);
    kprintf("removed %s (%d files)\n", name, n);
    return 0;
}

/* unpack an NPK into the filesystem root (used for the boot-time initrd) */
int pkg_extract_root(const u8 *d, size_t n, const char **err)
{
    int files = npk_walk("", d, n, 0, 1, err);
    if (files < 0) return -1;
    npk_walk("", d, n, 1, 1, err);
    return files;
}

/* find one file inside an NPK without installing it */
static int npk_find(const u8 *d, size_t n, const char *want, const u8 **out, size_t *olen)
{
    if (n < 5 || memcmp(d, "NPK1\n", 5)) return -1;
    size_t i = 5;
    while (i + 4 <= n) {
        if (!memcmp(d + i, "END\n", 4)) return -1;
        if (d[i] != 'F' || d[i + 1] != ' ') return -1;
        char rel[200]; size_t k = 0; i += 2;
        while (i < n && d[i] != ' ' && k < sizeof rel - 1) rel[k++] = (char)d[i++];
        rel[k] = 0; i++;
        u64 sz = 0; while (i < n && is_digit(d[i])) sz = sz * 10 + (d[i++] - '0');
        i++;
        if (i + sz + 1 > n) return -1;
        if (!strcmp(rel, want)) { *out = d + i; *olen = (size_t)sz; return 0; }
        i += (size_t)sz + 1;
    }
    return -1;
}

static void kv(const char *text, size_t n, const char *key, char *out, size_t on)
{
    size_t kl = strlen(key); out[0] = 0;
    for (size_t i = 0; i < n;) {
        size_t e = i; while (e < n && text[e] != '\n') e++;
        if (e - i > kl && !strncmp(text + i, key, kl) && text[i + kl] == '=') { size_t l = e - i - kl - 1; if (l >= on) l = on - 1; memcpy(out, text + i + kl + 1, l); out[l] = 0; return; }
        i = e + 1;
    }
}

/* a package shipped with the OS as an extra boot module: kept as a file in the offline cache, installed on demand (`pkg install name`) */
int pkg_stage_cache(const u8 *d, size_t n, const char **err)
{
    const u8 *pi; size_t pl;
    if (npk_find(d, n, ".pkginfo", &pi, &pl)) { *err = "no .pkginfo in the package"; return -1; }
    char name[24], ver[12], desc[64], file[64], path[96];
    kv((const char *)pi, pl, "name", name, sizeof name); kv((const char *)pi, pl, "version", ver, sizeof ver); kv((const char *)pi, pl, "description", desc, sizeof desc);
    if (!name[0] || !ver[0]) { *err = "bad .pkginfo"; return -1; }
    ksnprintf(file, sizeof file, "%s-%s.npk", name, ver);
    ksnprintf(path, sizeof path, "/var/cache/pkg/%s", file);
    if (fs_write(path, d, n)) { *err = "filesystem full"; return -1; }
    char h[65]; sha256_hex(d, n, h);
    const u8 *old; size_t on = 0; char line[256];
    int ll = ksnprintf(line, sizeof line, "%s %s %lu %s %s %s\n", name, ver, (u64)n, h, file, desc);
    if (fs_read("/var/cache/pkg/index.txt", &old, &on)) on = 0;
    char *nb = kmalloc(on + (size_t)ll + 1);
    if (!nb) { *err = "out of memory"; return -1; }
    if (on) memcpy(nb, old, on);
    memcpy(nb + on, line, (size_t)ll);
    fs_write("/var/cache/pkg/index.txt", nb, on + (size_t)ll);
    kfree(nb);
    return 0;
}

int pkg_installed_table(struct table *t)
{
    tb_init(t, "package", "version", "origin", NULL);
    char p[256]; size_t sz; int k = 0;
    for (int i = 0; !fs_info(i, p, sizeof p, &sz); i++) {
        if (strncmp(p, "/var/pkg/", 9)) continue;
        const u8 *d; size_t n; if (fs_read(p, &d, &n)) continue;
        char ver[32], org[100]; size_t a = 0; while (a < n && d[a] != '\n') a++;
        size_t vl = a < sizeof ver - 1 ? a : sizeof ver - 1; memcpy(ver, d, vl); ver[vl] = 0;
        org[0] = 0; if (a + 1 < n) { size_t b = a + 1; while (b < n && d[b] != '\n') b++; size_t ol = b - a - 1; if (ol >= sizeof org) ol = sizeof org - 1; memcpy(org, d + a + 1, ol); org[ol] = 0; }
        if (tb_newrow(t) < 0) break;
        tb_set(t, 0, "%s", p + 9); tb_set(t, 1, "%s", ver); tb_set(t, 2, "%s", org[0] ? org : "npk"); k++;
    }
    return k;
}
