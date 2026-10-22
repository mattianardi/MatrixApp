#include "fs.h"
#include "varbus.h"

#define MAXF 160
struct rfile { char path[96]; u8 *data; size_t size; int used; };
static struct rfile files[MAXF];
static size_t total;

void fs_init(void) { module_register("ramfs", "1.0", "in-memory filesystem; paths in Linux or Windows syntax"); }

void fs_norm(const char *in, char *out, size_t n)
{
    char tmp[160], cwd[96] = "/";
    if ((is_alpha(in[0]) && in[1] == ':') || strchr(in, '\\')) path_win_to_posix(in, tmp, sizeof tmp);
    else strlcpy(tmp, in, sizeof tmp);
    char full[256];
    if (tmp[0] == '/') strlcpy(full, tmp, sizeof full);
    else { bus_get("PWD", NEOS_VIEW_LINUX, cwd, sizeof cwd); ksnprintf(full, sizeof full, "%s/%s", cwd, tmp); }
    /* resolve '.', '..' and '//' */
    char *seg[32]; int ns = 0; char buf[256];
    strlcpy(buf, full, sizeof buf);
    for (char *p = buf; *p;) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *s = p; while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) { if (ns) ns--; continue; }
        if (ns < 32) seg[ns++] = s;
    }
    size_t l = 0;
    if (!ns) { strlcpy(out, "/", n); return; }
    for (int i = 0; i < ns; i++) l += ksnprintf(out + l, l < n ? n - l : 0, "/%s", seg[i]);
}

static struct rfile *find(const char *p)
{
    for (int i = 0; i < MAXF; i++) if (files[i].used && !strcmp(files[i].path, p)) return &files[i];
    return NULL;
}

int fs_write(const char *path, const void *data, size_t n)
{
    u8 *copy = kmalloc(n ? n : 1);
    if (!copy) return -1;
    memcpy(copy, data, n);
    struct rfile *f = find(path);
    if (!f) {
        for (int i = 0; i < MAXF; i++) if (!files[i].used) { f = &files[i]; break; }
        if (!f) { kfree(copy); return -2; }
        strlcpy(f->path, path, sizeof f->path);
        f->used = 1; f->data = NULL; f->size = 0;
    }
    u8 *old = f->data; total = total - f->size + n;
    f->data = copy; f->size = n;
    if (old) kfree(old);
    return 0;
}

int fs_read(const char *path, const u8 **data, size_t *n)
{
    struct rfile *f = find(path);
    if (!f) return -1;
    *data = f->data; *n = f->size;
    return 0;
}

int fs_remove(const char *path)
{
    struct rfile *f = find(path);
    if (!f) return -1;
    total -= f->size; kfree(f->data); f->used = 0; f->data = NULL;
    return 0;
}

int fs_remove_tree(const char *prefix)
{
    int n = 0; size_t pl = strlen(prefix);
    for (int i = 0; i < MAXF; i++)
        if (files[i].used && !strncmp(files[i].path, prefix, pl) && (files[i].path[pl] == '/' || !files[i].path[pl])) { fs_remove(files[i].path); n++; }
    return n;
}

int fs_count(void) { int n = 0; for (int i = 0; i < MAXF; i++) n += files[i].used; return n; }
int fs_info(int idx, char *path, size_t pn, size_t *size)
{
    for (int i = 0, k = 0; i < MAXF; i++) if (files[i].used && k++ == idx) { strlcpy(path, files[i].path, pn); *size = files[i].size; return 0; }
    return -1;
}
size_t fs_bytes(void) { return total; }
