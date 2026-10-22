#include "fs.h"
#include "vfs.h"
#include "varbus.h"

/* In-memory filesystem with real inodes: directories, in-place read/write/truncate, devices, symlinks. */
#define MAXI 8192
static struct inode nodes[MAXI];

/* path -> inode index: open addressing (value = index + 1, -1 = deleted). Keeps lookups O(1) with thousands of files (Python's stdlib, wheels). */
#define HSZ 32768
static int htab[HSZ];
static u32 hpath(const char *s) { u32 h = 2166136261u; for (; *s; s++) h = (h ^ (u8)*s) * 16777619u; return h; }
static void h_insert(int idx) { u32 h = hpath(nodes[idx].path) & (HSZ - 1); while (htab[h] > 0) h = (h + 1) & (HSZ - 1); htab[h] = idx + 1; }
static void h_remove(int idx)
{
    u32 h = hpath(nodes[idx].path) & (HSZ - 1);
    for (int n = 0; n < HSZ && htab[h]; n++, h = (h + 1) & (HSZ - 1)) if (htab[h] == idx + 1) { htab[h] = -1; return; }
}
static u32 next_ino = 2;
static size_t total;
static u64 boot_epoch;          /* unix time at boot (CMOS RTC) */

static int bcd(int v) { return (v & 15) + (v >> 4) * 10; }
static u64 rtc_epoch(void)
{
    #define CMOS(r) (outb(0x70, (r)), inb(0x71))
    while (CMOS(0x0A) & 0x80) { }
    int s = CMOS(0), m = CMOS(2), h = CMOS(4), d = CMOS(7), mo = CMOS(8), y = CMOS(9), regb = CMOS(0x0B);
    if (!(regb & 4)) { s = bcd(s); m = bcd(m); h = bcd(h & 0x7F) | (h & 0x80); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    int year = 2000 + y;
    static const int dm[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long days = (year - 1970) * 365L + (year - 1969) / 4 + dm[mo - 1] + d - 1;
    if (mo > 2 && year % 4 == 0) days++;
    return (u64)(((days * 24 + h) * 60 + m) * 60 + s);
}
u64 vfs_now(void) { return boot_epoch + time_ns() / 1000000000ull; }
u64 wall_ns(void) { return boot_epoch * 1000000000ull + time_ns(); }

void fill_random(void *buf, size_t n)
{
    u8 *b = buf;
    for (size_t k = 0; k < n; k += 8) {
        u64 r; if (hw_random(&r)) r = rdtsc() * 0x9E3779B97F4A7C15ull ^ ((u64)k << 17) ^ time_ns();
        for (size_t j = 0; j < 8 && k + j < n; j++) b[k + j] = (u8)(r >> (8 * j));
    }
}

static int (*proc_hook)(const char *, char *, size_t);
void vfs_register_proc_hook(int (*h)(const char *, char *, size_t)) { proc_hook = h; }

void fs_norm_at(const char *cwd, const char *in, char *out, size_t n)
{
    char tmp[256], full[512];
    if ((is_alpha(in[0]) && in[1] == ':' && (in[2] == '\\' || in[2] == '/' || !in[2])) || strchr(in, '\\')) path_win_to_posix(in, tmp, sizeof tmp);
    else strlcpy(tmp, in, sizeof tmp);
    if (tmp[0] == '/') strlcpy(full, tmp, sizeof full);
    else ksnprintf(full, sizeof full, "%s/%s", cwd, tmp);
    char *seg[48]; int ns = 0; char buf[512];
    strlcpy(buf, full, sizeof buf);
    for (char *p = buf; *p;) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *s = p; while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) { if (ns) ns--; continue; }
        if (ns < 48) seg[ns++] = s;
    }
    if (!ns) { strlcpy(out, "/", n); return; }
    size_t l = 0;
    for (int i = 0; i < ns; i++) l += ksnprintf(out + l, l < n ? n - l : 0, "/%s", seg[i]);
}

void fs_norm(const char *in, char *out, size_t n)
{
    char cwd[96] = "/";
    bus_get("PWD", NEOS_VIEW_LINUX, cwd, sizeof cwd);
    fs_norm_at(cwd, in, out, n);
}

static struct inode *lookup_raw(const char *path)
{
    u32 h = hpath(path) & (HSZ - 1);
    for (int n = 0; n < HSZ && htab[h]; n++, h = (h + 1) & (HSZ - 1))
        if (htab[h] > 0 && !strcmp(nodes[htab[h] - 1].path, path)) return &nodes[htab[h] - 1];
    return NULL;
}

static struct inode *alloc_node(const char *path, u32 mode);

struct inode *vfs_lookup(const char *path)
{
    if (!strncmp(path, "/proc/", 6) && proc_hook) {          /* synthetic files are (re)generated on lookup */
        char tmp[2048];
        int n = proc_hook(path, tmp, sizeof tmp);
        if (n >= 0) {
            struct inode *i = lookup_raw(path);
            if (!i) i = alloc_node(path, S_IFREG | 0444);
            if (!i) return NULL;
            inode_truncate(i, 0); inode_write(i, 0, tmp, (size_t)n);
            return i;
        }
    }
    return lookup_raw(path);
}

static struct inode *alloc_node(const char *path, u32 mode)
{
    for (int i = 0; i < MAXI; i++) if (!nodes[i].used) {
        struct inode *n = &nodes[i];
        memset(n, 0, sizeof *n);
        n->used = 1; n->mode = mode; n->ino = next_ino++; n->mtime = vfs_now();
        strlcpy(n->path, path, sizeof n->path);
        h_insert(i);
        return n;
    }
    return NULL;
}

static int ensure_parents(const char *path)
{
    char p[256];
    strlcpy(p, path, sizeof p);
    for (char *s = p + 1; *s; s++) if (*s == '/') {
        *s = 0;
        struct inode *d = lookup_raw(p);
        if (!d) { if (!alloc_node(p, S_IFDIR | 0755)) return -E_NOMEM; }
        else if ((d->mode & S_IFMT) != S_IFDIR) return -E_NOTDIR;
        *s = '/';
    }
    return 0;
}

int vfs_create(const char *path, u32 mode, struct inode **out)
{
    if (strlen(path) >= 255) return -E_NAMETOOLONG;
    struct inode *i = lookup_raw(path);
    if (i) { if (out) *out = i; return -E_EXIST; }
    int r = ensure_parents(path);
    if (r) return r;
    i = alloc_node(path, mode);
    if (!i) return -E_NOMEM;
    if (out) *out = i;
    return 0;
}

int vfs_mkdir(const char *path, u32 mode)
{
    if (vfs_lookup(path)) return -E_EXIST;
    int r = ensure_parents(path);
    if (r) return r;
    return alloc_node(path, S_IFDIR | (mode & 0777)) ? 0 : -E_NOMEM;
}

int vfs_readdir(const char *dir, int idx, char *name, size_t nn, int *type, u32 *ino)
{
    size_t dl = strlen(dir); if (dl == 1) dl = 0;
    int k = 0;
    for (int i = 0; i < MAXI; i++) {
        if (!nodes[i].used || strncmp(nodes[i].path, dir, dl) || nodes[i].path[dl] != '/' || !nodes[i].path[dl + 1]) continue;
        const char *rest = nodes[i].path + dl + 1;
        if (strchr(rest, '/')) continue;
        if (k++ == idx) { strlcpy(name, rest, nn); *type = (nodes[i].mode & S_IFMT); *ino = nodes[i].ino; return 0; }
    }
    return -1;
}

static void drop(struct inode *n)
{
    total -= n->size;
    if (n->data) kfree(n->data);
    h_remove((int)(n - nodes));
    n->used = 0; n->data = NULL;
}

int vfs_unlink(const char *path)
{
    struct inode *i = vfs_lookup(path);
    if (!i) return -E_NOENT;
    if ((i->mode & S_IFMT) == S_IFDIR) return -E_ISDIR;
    drop(i);
    return 0;
}

int vfs_rmdir(const char *path)
{
    struct inode *i = vfs_lookup(path);
    char nm[64]; int t; u32 in;
    if (!i) return -E_NOENT;
    if ((i->mode & S_IFMT) != S_IFDIR) return -E_NOTDIR;
    if (!vfs_readdir(path, 0, nm, sizeof nm, &t, &in)) return -E_NOTEMPTY;
    drop(i);
    return 0;
}

int vfs_rename(const char *from, const char *to)
{
    struct inode *a = vfs_lookup(from), *b = vfs_lookup(to);
    if (!a) return -E_NOENT;
    if (b) { if ((b->mode & S_IFMT) == S_IFDIR) return -E_ISDIR; drop(b); }
    int r = ensure_parents(to);
    if (r) return r;
    size_t fl = strlen(from);
    for (int i = 0; i < MAXI; i++) {
        struct inode *n = &nodes[i];
        if (!n->used) continue;
        if (n == a || (!strncmp(n->path, from, fl) && n->path[fl] == '/' && (a->mode & S_IFMT) == S_IFDIR)) {
            char np[256]; ksnprintf(np, sizeof np, "%s%s", to, n->path + fl);
            h_remove(i); strlcpy(n->path, np, sizeof n->path); h_insert(i);
        }
    }
    return 0;
}

int vfs_symlink(const char *target, const char *path)
{
    struct inode *i;
    int r = vfs_create(path, S_IFLNK | 0777, &i);
    if (r) return r;
    strlcpy(i->link, target, sizeof i->link);
    return 0;
}

long inode_read(struct inode *i, u64 off, void *buf, size_t n)
{
    switch (i->dev) {
    case DEV_NULL: return 0;
    case DEV_ZERO: memset(buf, 0, n); return (long)n;
    case DEV_RANDOM: {
        u8 *b = buf;
        for (size_t k = 0; k < n; k += 8) { u64 r; if (hw_random(&r)) r = rdtsc() * 0x9E3779B97F4A7C15ull ^ (u64)k; for (size_t j = 0; j < 8 && k + j < n; j++) b[k + j] = (u8)(r >> (8 * j)); }
        return (long)n; }
    }
    if (off >= i->size) return 0;
    if (n > i->size - off) n = i->size - off;
    memcpy(buf, i->data + off, n);
    return (long)n;
}

static int reserve(struct inode *i, size_t need)
{
    if (need <= i->cap) return 0;
    size_t nc = i->cap ? i->cap : 64;
    if (need >= (256u << 10) && !i->cap) nc = need;              /* first big write: exact size (the heap serves big blocks by pages) */
    else while (nc < need) nc *= 2;
    if (nc >= (256u << 10) && i->cap) { size_t g = i->cap + i->cap / 2; if (g >= need) nc = g; }   /* big files grow by 1.5x */
    u8 *nd = kmalloc(nc);
    if (!nd) return -E_NOMEM;
    if (i->size) memcpy(nd, i->data, i->size);
    if (i->data) kfree(i->data);
    i->data = nd; i->cap = nc;
    return 0;
}

long inode_write(struct inode *i, u64 off, const void *buf, size_t n)
{
    if (i->dev == DEV_NULL || i->dev == DEV_ZERO || i->dev == DEV_RANDOM) return (long)n;
    if (off + n < off || off + n > (256ull << 20)) return -E_INVAL;
    int r = reserve(i, off + n);
    if (r) return r;
    if (off > i->size) memset(i->data + i->size, 0, off - i->size);
    memcpy(i->data + off, buf, n);
    if (off + n > i->size) { total += off + n - i->size; i->size = off + n; }
    i->mtime = vfs_now();
    return (long)n;
}

int inode_truncate(struct inode *i, u64 len)
{
    if (len > (256ull << 20)) return -E_INVAL;
    if (len > i->size) {
        int r = reserve(i, len);
        if (r) return r;
        memset(i->data + i->size, 0, len - i->size);
    }
    total = total - i->size + len;
    i->size = len;
    return 0;
}

/* ---- legacy flat API used by the shell, package manager and agents ---- */
void fs_init(void)
{
    module_register("ramfs", "2.0", "in-memory filesystem: inodes, directories, devices, symlinks; Linux or Windows path syntax");
    boot_epoch = rtc_epoch() - time_ns() / 1000000000ull;
    alloc_node("/", S_IFDIR | 0755);
    static const struct { const char *p; int dev; } devs[] = { { "/dev/null", DEV_NULL }, { "/dev/zero", DEV_ZERO }, { "/dev/urandom", DEV_RANDOM },
        { "/dev/random", DEV_RANDOM }, { "/dev/tty", DEV_TTY }, { "/dev/console", DEV_TTY } };
    for (unsigned k = 0; k < sizeof devs / sizeof *devs; k++) { struct inode *i; if (!vfs_create(devs[k].p, S_IFCHR | 0666, &i)) i->dev = devs[k].dev; }
    vfs_mkdir("/tmp", 01777); vfs_mkdir("/proc", 0555); vfs_mkdir("/mnt/c", 0755); vfs_mkdir("/etc", 0755); vfs_mkdir("/home", 0755); vfs_mkdir("/root", 0700);
}

int fs_write(const char *path, const void *data, size_t n)
{
    struct inode *i = vfs_lookup(path);
    if (!i) { int r = vfs_create(path, S_IFREG | 0644, &i); if (r) return -1; }
    if ((i->mode & S_IFMT) == S_IFDIR) return -1;
    if (inode_truncate(i, 0)) return -1;
    return inode_write(i, 0, data, n) < 0 ? -1 : 0;
}

int fs_read(const char *path, const u8 **data, size_t *n)
{
    struct inode *i = vfs_lookup(path);
    if (!i || (i->mode & S_IFMT) != S_IFREG || i->dev) return -1;
    *data = i->data ? i->data : (const u8 *)""; *n = i->size;
    return 0;
}

int fs_remove(const char *path) { return vfs_unlink(path) ? -1 : 0; }

int fs_remove_tree(const char *prefix)
{
    int n = 0; size_t pl = strlen(prefix);
    for (int i = 0; i < MAXI; i++)
        if (nodes[i].used && !strncmp(nodes[i].path, prefix, pl) && (nodes[i].path[pl] == '/' || !nodes[i].path[pl])) { n += (nodes[i].mode & S_IFMT) == S_IFREG; drop(&nodes[i]); }
    return n;
}

int fs_count(void) { int n = 0; for (int i = 0; i < MAXI; i++) n += nodes[i].used && (nodes[i].mode & S_IFMT) == S_IFREG && !nodes[i].dev; return n; }

int fs_info(int idx, char *path, size_t pn, size_t *size)
{
    for (int i = 0, k = 0; i < MAXI; i++)
        if (nodes[i].used && (nodes[i].mode & S_IFMT) == S_IFREG && !nodes[i].dev && k++ == idx) { strlcpy(path, nodes[i].path, pn); *size = nodes[i].size; return 0; }
    return -1;
}

size_t fs_bytes(void) { return total; }

/* every live inode except devices and synthetic /proc files, parents before children is NOT guaranteed (callers create parents on demand) */
void vfs_walk(void (*cb)(struct inode *i, void *ctx), void *ctx)
{
    for (int i = 0; i < MAXI; i++) if (nodes[i].used && !nodes[i].dev && strncmp(nodes[i].path, "/proc", 5)) cb(&nodes[i], ctx);
}
