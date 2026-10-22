#include "fs.h"
#include "net.h"
#include "shell.h"

/*
 * Package manager. Repository layout (see tools/mkrepo.py):
 *   <repo>/index.txt : "name version size sha256 file description..."
 *   <repo>/<file>    : NPK container  "NPK1\n" { "F <relpath> <size>\n" <bytes> "\n" } "END\n"
 * Files are installed under /opt/<name>/ and recorded in /var/pkg/<name>.
 * Integrity: package size+SHA-256 come from the index; the index itself can be pinned with
 * pkg.index_sha256. Transport is plain HTTP until TLS exists, so it is refused unless
 * pkg.allow_insecure is on.
 */
#define MAXP 64
struct pent { char name[24], ver[12], sha[65], file[48], desc[64]; size_t size; };
static struct pent idx[MAXP];
static int nidx;
static struct tunable *t_repo, *t_insecure, *t_pin, *t_maxkib;

void pkg_init(void)
{
    module_register("pkg", "0.1", "package manager: HTTP repository, SHA-256 verified NPK packages");
    t_repo     = tune_str ("pkg", "repo", "repository base URL", TF_LIVE, "http://10.0.2.2:8080", NULL);
    t_insecure = tune_bool("pkg", "allow_insecure", "allow plain-HTTP repositories (no TLS yet)", TF_LIVE, 0, NULL);
    t_pin      = tune_str ("pkg", "index_sha256", "if set, `pkg update` rejects an index with a different SHA-256", TF_LIVE, "", NULL);
    t_maxkib   = tune_int ("pkg", "max_kib", "largest package/index accepted (KiB)", TF_LIVE, 1024, 16, 8192, NULL);
}

static int check_transport(const char **err)
{
    if (!strncmp(t_repo->sval, "https://", 8)) { *err = "https:// repositories need TLS, which NEOS does not have yet"; return -1; }
    if (!t_insecure->val) { *err = "plain-HTTP repository refused: `tune set pkg.allow_insecure on` to accept the risk (pin the index with pkg.index_sha256)"; return -1; }
    return 0;
}

static int fetch(const char *name, void **buf, size_t *len, const char **err)
{
    char url[200];
    size_t cap = (size_t)t_maxkib->val * 1024;
    ksnprintf(url, sizeof url, "%s/%s", t_repo->sval, name);
    *buf = kmalloc(cap + 1);
    if (!*buf) { *err = "out of memory"; return -1; }
    int st = 0;
    if (http_get(url, *buf, cap, len, &st, err)) { kfree(*buf); return -1; }
    if (st != 200) { static char m[32]; ksnprintf(m, sizeof m, "server answered HTTP %d", st); *err = m; kfree(*buf); return -1; }
    return 0;
}

static int installed(const char *name)
{
    char p[64]; const u8 *d; size_t n;
    ksnprintf(p, sizeof p, "/var/pkg/%s", name);
    return fs_read(p, &d, &n) == 0;
}

int pkg_update(const char **err)
{
    void *b; size_t n;
    if (check_transport(err)) return -1;
    if (fetch("index.txt", &b, &n, err)) return -1;
    char h[65]; sha256_hex(b, n, h);
    if (t_pin->sval[0] && strcasecmp(h, t_pin->sval)) { *err = "index SHA-256 does not match pkg.index_sha256"; kfree(b); return -1; }
    char *s = b; s[n] = 0;
    nidx = 0;
    for (char *l = s, *e; *l && nidx < MAXP; l = e) {
        e = strchr(l, '\n'); if (e) *e++ = 0; else e = l + strlen(l);
        if (*l == '#' || !*l) continue;
        struct pent *p = &idx[nidx]; char sz[16] = "";
        char *f[5]; int k = 0;
        for (char *q = l; *q && k < 5;) {
            while (*q == ' ') q++;
            if (!*q) break;
            f[k++] = q; while (*q && *q != ' ') q++;
            if (*q) *q++ = 0;
            if (k == 5) { strlcpy(p->desc, q, sizeof p->desc); break; }
        }
        if (k < 5) continue;
        strlcpy(p->name, f[0], sizeof p->name); strlcpy(p->ver, f[1], sizeof p->ver);
        strlcpy(sz, f[2], sizeof sz); strlcpy(p->sha, f[3], sizeof p->sha); strlcpy(p->file, f[4], sizeof p->file);
        i64 v; if (parse_i64(sz, &v) || v < 0) continue;
        p->size = (size_t)v;
        nidx++;
    }
    kfree(b);
    kprintf("index %s (sha256 %s): %d packages\n", t_repo->sval, h, nidx);
    return 0;
}

int pkg_table(struct table *t)
{
    tb_init(t, "package", "version", "size", "state", "description", NULL);
    for (int i = 0; i < nidx; i++) {
        if (tb_newrow(t) < 0) break;
        tb_set(t, 0, "%s", idx[i].name); tb_set(t, 1, "%s", idx[i].ver); tb_set(t, 2, "%lu", (u64)idx[i].size);
        tb_set(t, 3, "%s", installed(idx[i].name) ? "installed" : "-"); tb_set(t, 4, "%s", idx[i].desc);
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
static int npk_walk(const char *root, const u8 *d, size_t n, int write, const char **err)
{
    if (n < 5 || memcmp(d, "NPK1\n", 5)) { *err = "not an NPK package"; return -1; }
    size_t i = 5; int files = 0;
    while (i < n) {
        if (n - i >= 4 && !memcmp(d + i, "END\n", 4)) return files;
        if (d[i] != 'F' || d[i + 1] != ' ') { *err = "corrupt package"; return -1; }
        char rel[64]; size_t k = 0; i += 2;
        while (i < n && d[i] != ' ' && k < sizeof rel - 1) rel[k++] = (char)d[i++];
        rel[k] = 0;
        if (i >= n || d[i] != ' ') { *err = "corrupt package"; return -1; }
        i++;
        u64 sz = 0; while (i < n && is_digit(d[i])) sz = sz * 10 + (d[i++] - '0');
        if (i >= n || d[i] != '\n' || sz > n || i + 1 + sz + 1 > n) { *err = "corrupt package"; return -1; }
        i++;
        if (!rel[0] || rel[0] == '/' || has_dotdot(rel)) { *err = "package contains an unsafe path"; return -1; }
        if (write) {
            char path[128]; ksnprintf(path, sizeof path, "%s/%s", root, rel);
            if (fs_write(path, d + i, (size_t)sz)) { *err = "filesystem full"; return -1; }
        }
        i += (size_t)sz + 1;
        files++;
    }
    *err = "package has no END marker";
    return -1;
}

/* validate the whole package first, then replace /opt/<name>; returns file count or -1 */
int pkg_install_blob(const char *name, const char *ver, const u8 *d, size_t n, const char **err)
{
    char root[64], rec[64];
    ksnprintf(root, sizeof root, "/opt/%s", name);
    int files = npk_walk(root, d, n, 0, err);
    if (files < 0) return -1;
    fs_remove_tree(root);
    npk_walk(root, d, n, 1, err);
    ksnprintf(rec, sizeof rec, "/var/pkg/%s", name);
    fs_write(rec, ver, strlen(ver));
    return files;
}

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

int pkg_install(const char *name, const char **err)
{
    struct pent *p = NULL;
    if (check_transport(err)) return -1;
    for (int i = 0; i < nidx; i++) if (!strcmp(idx[i].name, name)) p = &idx[i];
    if (!p) { *err = "unknown package (run `pkg update`, then `pkg list`)"; return -1; }
    void *b; size_t n;
    kprintf("fetching %s %s (%lu bytes)...\n", p->name, p->ver, (u64)p->size);
    if (fetch(p->file, &b, &n, err)) return -1;
    char h[65]; sha256_hex(b, n, h);
    if (n != p->size) { *err = "size differs from the index"; kfree(b); return -1; }
    if (strcasecmp(h, p->sha)) { *err = "SHA-256 mismatch: package corrupted or tampered"; kfree(b); return -1; }
    int files = pkg_install_blob(p->name, p->ver, b, n, err);
    kfree(b);
    if (files < 0) return -1;
    kprintf("installed %s %s: %d files in /opt/%s (sha256 verified)\n", p->name, p->ver, files, p->name);
    return 0;
}

int pkg_remove(const char *name, const char **err)
{
    char root[64], rec[64];
    ksnprintf(root, sizeof root, "/opt/%s", name); ksnprintf(rec, sizeof rec, "/var/pkg/%s", name);
    if (!installed(name)) { *err = "not installed"; return -1; }
    int n = fs_remove_tree(root);
    fs_remove(rec);
    kprintf("removed %s (%d files)\n", name, n);
    return 0;
}

/* unpack an NPK into the filesystem root (used for the boot-time initrd) */
int pkg_extract_root(const u8 *d, size_t n, const char **err)
{
    int files = npk_walk("", d, n, 0, err);
    if (files < 0) return -1;
    npk_walk("", d, n, 1, err);
    return files;
}
