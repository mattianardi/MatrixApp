/* winrt extensions: registry (advapi32), Winsock (ws2_32), synchronisation primitives of the newer Windows runtimes (SRW locks, condition
 * variables, one-time init, fibre-local storage), multimedia timers, COM start-up, and a handful of system queries. */
#include "winrt.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/select.h>

typedef LONG LSTATUS;
typedef uintptr_t SOCKET;

static size_t w2u(const WCHAR *w, char *out, size_t cap)            /* UTF-16 -> UTF-8 */
{
    size_t l = 0;
    for (; *w && l + 5 < cap; w++) {
        uint32_t c = *w;
        if (c >= 0xD800 && c < 0xDC00 && w[1] >= 0xDC00 && w[1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (w[1] - 0xDC00); w++; }
        if (c < 0x80) out[l++] = (char)c;
        else if (c < 0x800) { out[l++] = (char)(0xC0 | c >> 6); out[l++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out[l++] = (char)(0xE0 | c >> 12); out[l++] = (char)(0x80 | (c >> 6 & 0x3F)); out[l++] = (char)(0x80 | (c & 0x3F)); }
        else { out[l++] = (char)(0xF0 | c >> 18); out[l++] = (char)(0x80 | (c >> 12 & 0x3F)); out[l++] = (char)(0x80 | (c >> 6 & 0x3F)); out[l++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[l] = 0; return l;
}
static size_t u2w(const char *s, WCHAR *out, size_t cap)            /* UTF-8 -> UTF-16, returns units written (without the terminator) */
{
    size_t l = 0; const unsigned char *p = (const unsigned char *)s;
    while (*p && l + 3 < cap) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 0x3F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 0x3F); p++; }
        if (c >= 0x10000) { c -= 0x10000; out[l++] = (WCHAR)(0xD800 + (c >> 10)); out[l++] = (WCHAR)(0xDC00 + (c & 0x3FF)); } else out[l++] = (WCHAR)c;
    }
    out[l] = 0; return l;
}

/* ================================================================ registry
 * Keys are directories under C:\ProgramData\neos-registry\<HKLM|HKCU|HKCR|HKU>\..., values live in the file "@values" of the key
 * (name TAB type TAB hex bytes): the registry therefore persists like any other file of drive C:. */
#define HK_CLASSES_ROOT   ((HANDLE)(uintptr_t)0x80000000u)
#define HK_CURRENT_USER   ((HANDLE)(uintptr_t)0x80000001u)
#define HK_LOCAL_MACHINE  ((HANDLE)(uintptr_t)0x80000002u)
#define HK_USERS          ((HANDLE)(uintptr_t)0x80000003u)
#define HK_CURRENT_CONFIG ((HANDLE)(uintptr_t)0x80000005u)
#define REG_ROOT "/mnt/c/ProgramData/neos-registry"
#define MAXKEYS 256
static struct { int used; char dir[512]; } rkeys[MAXKEYS];
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static int reg_seeded;

static void mkdirs(const char *p)
{
    char t[600]; strlcpy(t, p, sizeof t);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}
static int rd_values(const char *dir, char (*names)[256], DWORD *types, uint8_t **data, DWORD *lens, int max);
static void put_value(const char *dir, const char *name, DWORD type, const void *d, DWORD n);

static void reg_seed(void)
{
    if (reg_seeded) return;
    reg_seeded = 1;
    char d[600];
    snprintf(d, sizeof d, REG_ROOT "/HKLM/SOFTWARE/Microsoft/Windows NT/CurrentVersion");
    if (access(d, F_OK) == 0) return;
    mkdirs(d);
    static const struct { const char *n, *v; } sv[] = { { "ProductName", "NEOS Windows API runtime" }, { "CurrentVersion", "10.0" }, { "CurrentBuildNumber", "19045" },
        { "SystemRoot", "C:\\Windows" }, { "ProgramFilesDir", "C:\\Program Files" }, { "EditionID", "Professional" } };
    for (unsigned i = 0; i < sizeof sv / sizeof *sv; i++) put_value(d, sv[i].n, 1, sv[i].v, (DWORD)strlen(sv[i].v) + 1);
    snprintf(d, sizeof d, REG_ROOT "/HKLM/SOFTWARE/Microsoft/Windows/CurrentVersion");
    mkdirs(d);
    put_value(d, "ProgramFilesDir", 1, "C:\\Program Files", 17); put_value(d, "CommonFilesDir", 1, "C:\\Program Files\\Common Files", 30);
    snprintf(d, sizeof d, REG_ROOT "/HKLM/SYSTEM/CurrentControlSet/Control/Session Manager/Environment"); mkdirs(d);
    snprintf(d, sizeof d, REG_ROOT "/HKCU/Environment"); mkdirs(d);
}

static const char *hive_dir(HANDLE h)
{
    uintptr_t hv = (uintptr_t)h; if (hv >= 0xFFFFFFFF00000000ull) hv = (uint32_t)hv;          /* predefined keys arrive sign-extended */
    switch (hv) {
    case 0x80000000u: return REG_ROOT "/HKCR"; case 0x80000001u: return REG_ROOT "/HKCU"; case 0x80000002u: return REG_ROOT "/HKLM";
    case 0x80000003u: return REG_ROOT "/HKU"; case 0x80000005u: return REG_ROOT "/HKLM/SYSTEM/CurrentControlSet/Hardware Profiles/Current";
    }
    return NULL;
}
static const char *key_dir(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (v >= 0x1000 && v < 0x1000 + MAXKEYS && rkeys[v - 0x1000].used) return rkeys[v - 0x1000].dir;
    return hive_dir(h);
}
static HANDLE new_key(const char *dir)
{
    for (int i = 0; i < MAXKEYS; i++) if (!rkeys[i].used) { rkeys[i].used = 1; strlcpy(rkeys[i].dir, dir, sizeof rkeys[i].dir); return (HANDLE)(uintptr_t)(0x1000 + i); }
    return NULL;
}
static void sub_path(char *out, size_t cap, const char *base, const char *sub)
{
    size_t l = strlcpy(out, base, cap);
    if (sub && *sub && l + 1 < cap) out[l++] = '/';
    for (const char *s = sub; s && *s && l + 1 < cap; s++) { if (*s == '\\' || *s == '/') { if (out[l - 1] != '/') out[l++] = '/'; } else out[l++] = *s; }
    out[l] = 0;
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static LSTATUS reg_open_a(HANDLE parent, const char *sub, int create, HANDLE *out, DWORD *disp)
{
    pthread_mutex_lock(&reg_lock); reg_seed();
    const char *base = key_dir(parent); char d[600];
    if (!base) { pthread_mutex_unlock(&reg_lock); return 6; }
    sub_path(d, sizeof d, base, sub);
    int existed = is_dir(d);
    if (!existed) {
        if (!create) { pthread_mutex_unlock(&reg_lock); return 2; }
        mkdirs(d);
    }
    if (disp) *disp = existed ? 2 : 1;
    HANDLE h = (sub && *sub) || !hive_dir(parent) ? new_key(d) : parent;
    if (!h) { pthread_mutex_unlock(&reg_lock); return 4; }
    *out = h; pthread_mutex_unlock(&reg_lock);
    return 0;
}

/* value file: one line per value: name \t type \t hex */
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; }
static int rd_values(const char *dir, char (*names)[256], DWORD *types, uint8_t **data, DWORD *lens, int max)
{
    char f[640]; snprintf(f, sizeof f, "%s/@values", dir);
    FILE *fp = fopen(f, "r"); if (!fp) return 0;
    int n = 0; char *line = NULL; size_t cap = 0; ssize_t ll;
    while (n < max && (ll = getline(&line, &cap, fp)) > 0) {
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1++ = 0;
        char *t2 = strchr(t1, '\t'); if (!t2) continue; *t2++ = 0;
        strlcpy(names[n], line, 256); types[n] = (DWORD)strtoul(t1, NULL, 10);
        size_t hl = strcspn(t2, "\r\n"); lens[n] = (DWORD)(hl / 2); data[n] = malloc(lens[n] + 2);
        for (DWORD i = 0; i < lens[n]; i++) data[n][i] = (uint8_t)(hexv(t2[2 * i]) << 4 | hexv(t2[2 * i + 1]));
        n++;
    }
    free(line); fclose(fp); return n;
}
static void wr_values(const char *dir, int n, char (*names)[256], DWORD *types, uint8_t **data, DWORD *lens)
{
    char f[640]; snprintf(f, sizeof f, "%s/@values", dir);
    FILE *fp = fopen(f, "w"); if (!fp) return;
    for (int i = 0; i < n; i++) {
        fprintf(fp, "%s\t%u\t", names[i], types[i]);
        for (DWORD k = 0; k < lens[i]; k++) fprintf(fp, "%02x", data[i][k]);
        fputc('\n', fp);
    }
    fclose(fp);
}
#define MAXV 512
static int load_all(const char *dir, char (**names)[256], DWORD **types, uint8_t ***data, DWORD **lens)
{
    *names = calloc(MAXV, 256); *types = calloc(MAXV, 4); *data = calloc(MAXV, sizeof(uint8_t *)); *lens = calloc(MAXV, 4);
    return rd_values(dir, *names, *types, *data, *lens, MAXV);
}
static void free_all(int n, char (*names)[256], DWORD *types, uint8_t **data, DWORD *lens)
{
    for (int i = 0; i < n; i++) free(data[i]);
    free(names); free(types); free(data); free(lens);
}
static void put_value(const char *dir, const char *name, DWORD type, const void *d, DWORD len)
{
    char (*names)[256]; DWORD *types, *lens; uint8_t **data;
    int n = load_all(dir, &names, &types, &data, &lens), k = -1;
    for (int i = 0; i < n; i++) if (!strcasecmp(names[i], name)) k = i;
    if (k < 0 && n < MAXV) k = n++;
    if (k >= 0) {
        free(data[k]); strlcpy(names[k], name, 256); types[k] = type; lens[k] = len; data[k] = malloc(len + 2); memcpy(data[k], d, len);
        wr_values(dir, n, names, types, data, lens);
    }
    free_all(n, names, types, data, lens);
}

#define ASTR(w) char w##_a[1024]; w2u((w) ? (w) : (const WCHAR *)u"", w##_a, sizeof w##_a)
WINAPI static LSTATUS e_RegOpenKeyExA(HANDLE k, LPCSTR sub, DWORD o, DWORD sam, HANDLE *out) { (void)o; (void)sam; return reg_open_a(k, sub, 0, out, NULL); }
WINAPI static LSTATUS e_RegOpenKeyExW(HANDLE k, LPCWSTR sub, DWORD o, DWORD sam, HANDLE *out) { (void)o; (void)sam; ASTR(sub); return reg_open_a(k, sub_a, 0, out, NULL); }
WINAPI static LSTATUS e_RegOpenKeyA(HANDLE k, LPCSTR sub, HANDLE *out) { return reg_open_a(k, sub, 0, out, NULL); }
WINAPI static LSTATUS e_RegCreateKeyExA(HANDLE k, LPCSTR sub, DWORD r, LPSTR cls, DWORD opt, DWORD sam, void *sa, HANDLE *out, DWORD *disp) { (void)r; (void)cls; (void)opt; (void)sam; (void)sa; return reg_open_a(k, sub, 1, out, disp); }
WINAPI static LSTATUS e_RegCreateKeyExW(HANDLE k, LPCWSTR sub, DWORD r, LPWSTR cls, DWORD opt, DWORD sam, void *sa, HANDLE *out, DWORD *disp) { (void)r; (void)cls; (void)opt; (void)sam; (void)sa; ASTR(sub); return reg_open_a(k, sub_a, 1, out, disp); }
WINAPI static LSTATUS e_RegCreateKeyA(HANDLE k, LPCSTR sub, HANDLE *out) { return reg_open_a(k, sub, 1, out, NULL); }
WINAPI static LSTATUS e_RegCloseKey(HANDLE k)
{
    uintptr_t v = (uintptr_t)k;
    if (v >= 0x1000 && v < 0x1000 + MAXKEYS) { pthread_mutex_lock(&reg_lock); rkeys[v - 0x1000].used = 0; pthread_mutex_unlock(&reg_lock); return 0; }
    return hive_dir(k) ? 0 : 6;
}
WINAPI static LSTATUS e_RegFlushKey(HANDLE k) { (void)k; return 0; }

static LSTATUS query_value(HANDLE k, const char *name, DWORD *type, void *buf, DWORD *len, int wide_out_unused)
{
    (void)wide_out_unused;
    const char *dir = key_dir(k); if (!dir) return 6;
    pthread_mutex_lock(&reg_lock);
    char (*names)[256]; DWORD *types, *lens; uint8_t **data;
    int n = load_all(dir, &names, &types, &data, &lens), hit = -1;
    for (int i = 0; i < n; i++) if (!strcasecmp(names[i], name ? name : "")) hit = i;
    LSTATUS rc = 2;
    if (hit >= 0) {
        if (type) *type = types[hit];
        rc = 0;
        if (buf && len) { if (*len < lens[hit]) rc = 234; else memcpy(buf, data[hit], lens[hit]); }
        else if (buf && !len) rc = 87;
        if (len) *len = lens[hit];
    }
    free_all(n, names, types, data, lens); pthread_mutex_unlock(&reg_lock);
    return rc;
}
WINAPI static LSTATUS e_RegQueryValueExA(HANDLE k, LPCSTR name, DWORD *res, DWORD *type, void *buf, DWORD *len) { (void)res; return query_value(k, name, type, buf, len, 0); }
WINAPI static LSTATUS e_RegQueryValueExW(HANDLE k, LPCWSTR name, DWORD *res, DWORD *type, void *buf, DWORD *len) { (void)res; ASTR(name); return query_value(k, name_a, type, buf, len, 1); }
WINAPI static LSTATUS e_RegQueryValueA(HANDLE k, LPCSTR sub, LPSTR buf, LONG *len)
{
    HANDLE h; LSTATUS r = reg_open_a(k, sub, 0, &h, NULL); if (r) return r;
    DWORD t, l = len ? (DWORD)*len : 0; r = query_value(h, "", &t, buf, &l, 0); if (len) *len = (LONG)l; e_RegCloseKey(h); return r;
}
static LSTATUS set_value(HANDLE k, const char *name, DWORD type, const void *d, DWORD n)
{
    const char *dir = key_dir(k); if (!dir) return 6;
    pthread_mutex_lock(&reg_lock); put_value(dir, name ? name : "", type, d, n); pthread_mutex_unlock(&reg_lock); return 0;
}
WINAPI static LSTATUS e_RegSetValueExA(HANDLE k, LPCSTR name, DWORD r, DWORD type, const void *d, DWORD n) { (void)r; return set_value(k, name, type, d, n); }
WINAPI static LSTATUS e_RegSetValueExW(HANDLE k, LPCWSTR name, DWORD r, DWORD type, const void *d, DWORD n) { (void)r; ASTR(name); return set_value(k, name_a, type, d, n); }
WINAPI static LSTATUS e_RegSetValueA(HANDLE k, LPCSTR sub, DWORD type, LPCSTR d, DWORD n)
{
    (void)type; HANDLE h; LSTATUS r = reg_open_a(k, sub, 1, &h, NULL); if (r) return r;
    r = set_value(h, "", 1, d, n ? n + 1 : (DWORD)strlen(d) + 1); e_RegCloseKey(h); return r;
}
static LSTATUS del_value(HANDLE k, const char *name)
{
    const char *dir = key_dir(k); if (!dir) return 6;
    pthread_mutex_lock(&reg_lock);
    char (*names)[256]; DWORD *types, *lens; uint8_t **data;
    int n = load_all(dir, &names, &types, &data, &lens), hit = -1;
    for (int i = 0; i < n; i++) if (!strcasecmp(names[i], name ? name : "")) hit = i;
    LSTATUS rc = 2;
    if (hit >= 0) {
        free(data[hit]);
        for (int i = hit; i + 1 < n; i++) { memcpy(names[i], names[i + 1], 256); types[i] = types[i + 1]; data[i] = data[i + 1]; lens[i] = lens[i + 1]; }
        data[n - 1] = NULL; wr_values(dir, n - 1, names, types, data, lens); rc = 0;
    }
    free_all(n, names, types, data, lens); pthread_mutex_unlock(&reg_lock);
    return rc;
}
WINAPI static LSTATUS e_RegDeleteValueA(HANDLE k, LPCSTR name) { return del_value(k, name); }
WINAPI static LSTATUS e_RegDeleteValueW(HANDLE k, LPCWSTR name) { ASTR(name); return del_value(k, name_a); }
static LSTATUS del_key(HANDLE k, const char *sub)
{
    pthread_mutex_lock(&reg_lock);
    const char *base = key_dir(k); char d[600], f[700];
    if (!base) { pthread_mutex_unlock(&reg_lock); return 6; }
    sub_path(d, sizeof d, base, sub);
    snprintf(f, sizeof f, "%s/@values", d); unlink(f);
    int r = rmdir(d);
    pthread_mutex_unlock(&reg_lock);
    return r == 0 ? 0 : errno == ENOENT ? 2 : 5;
}
WINAPI static LSTATUS e_RegDeleteKeyA(HANDLE k, LPCSTR sub) { return del_key(k, sub); }
WINAPI static LSTATUS e_RegDeleteKeyW(HANDLE k, LPCWSTR sub) { ASTR(sub); return del_key(k, sub_a); }
WINAPI static LSTATUS e_RegDeleteTreeA(HANDLE k, LPCSTR sub)
{
    pthread_mutex_lock(&reg_lock); const char *base = key_dir(k); char d[600], cmd[700];
    if (!base) { pthread_mutex_unlock(&reg_lock); return 6; }
    sub_path(d, sizeof d, base, sub); snprintf(cmd, sizeof cmd, "rm -rf '%s'", d); int r = system(cmd); pthread_mutex_unlock(&reg_lock); return r == 0 ? 0 : 5;
}
static LSTATUS enum_key(HANDLE k, DWORD idx, char *out, DWORD *len)
{
    const char *dir = key_dir(k); if (!dir) return 6;
    DIR *d = opendir(dir); if (!d) return 259;
    struct dirent *e; DWORD i = 0; LSTATUS rc = 259;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_name[0] == '@') continue;
        if (i++ == idx) { DWORD l = (DWORD)strlen(e->d_name); if (*len <= l) rc = 234; else { memcpy(out, e->d_name, l + 1); *len = l; rc = 0; } break; }
    }
    closedir(d); return rc;
}
WINAPI static LSTATUS e_RegEnumKeyExA(HANDLE k, DWORD idx, LPSTR name, DWORD *len, DWORD *res, LPSTR cls, DWORD *clen, void *ft) { (void)res; (void)cls; (void)clen; (void)ft; return enum_key(k, idx, name, len); }
WINAPI static LSTATUS e_RegEnumKeyExW(HANDLE k, DWORD idx, LPWSTR name, DWORD *len, DWORD *res, LPWSTR cls, DWORD *clen, void *ft)
{
    (void)res; (void)cls; (void)clen; (void)ft; char t[300]; DWORD l = sizeof t; LSTATUS r = enum_key(k, idx, t, &l); if (r) return r;
    size_t w = u2w(t, name, *len); *len = (DWORD)w; return 0;
}
WINAPI static LSTATUS e_RegEnumKeyA(HANDLE k, DWORD idx, LPSTR name, DWORD len) { DWORD l = len; return enum_key(k, idx, name, &l); }
static LSTATUS enum_value(HANDLE k, DWORD idx, char *name, DWORD *nlen, DWORD *type, uint8_t *data, DWORD *dlen)
{
    const char *dir = key_dir(k); if (!dir) return 6;
    pthread_mutex_lock(&reg_lock);
    char (*names)[256]; DWORD *types, *lens; uint8_t **dat;
    int n = load_all(dir, &names, &types, &dat, &lens); LSTATUS rc = 259;
    if ((int)idx < n) {
        DWORD l = (DWORD)strlen(names[idx]); rc = 0;
        if (*nlen <= l) rc = 234; else { memcpy(name, names[idx], l + 1); *nlen = l; }
        if (type) *type = types[idx];
        if (dlen) { if (data && *dlen < lens[idx]) rc = 234; else if (data) memcpy(data, dat[idx], lens[idx]); *dlen = lens[idx]; }
    }
    free_all(n, names, types, dat, lens); pthread_mutex_unlock(&reg_lock);
    return rc;
}
WINAPI static LSTATUS e_RegEnumValueA(HANDLE k, DWORD idx, LPSTR name, DWORD *nlen, DWORD *res, DWORD *type, uint8_t *data, DWORD *dlen) { (void)res; return enum_value(k, idx, name, nlen, type, data, dlen); }
WINAPI static LSTATUS e_RegEnumValueW(HANDLE k, DWORD idx, LPWSTR name, DWORD *nlen, DWORD *res, DWORD *type, uint8_t *data, DWORD *dlen)
{
    (void)res; char t[300]; DWORD l = sizeof t; LSTATUS r = enum_value(k, idx, t, &l, type, data, dlen); if (r) return r;
    size_t w = u2w(t, name, *nlen); *nlen = (DWORD)w; return 0;
}
WINAPI static LSTATUS e_RegQueryInfoKeyA(HANDLE k, LPSTR cls, DWORD *clen, DWORD *res, DWORD *nsub, DWORD *maxsub, DWORD *maxcls, DWORD *nval, DWORD *maxname, DWORD *maxdata, DWORD *secd, void *ft)
{
    (void)cls; (void)clen; (void)res; (void)maxcls; (void)secd; (void)ft;
    const char *dir = key_dir(k); if (!dir) return 6;
    DWORD ns = 0, ms = 0; DIR *d = opendir(dir); struct dirent *e;
    if (d) { while ((e = readdir(d))) if (e->d_name[0] != '.' && e->d_name[0] != '@') { ns++; DWORD l = (DWORD)strlen(e->d_name); if (l > ms) ms = l; } closedir(d); }
    pthread_mutex_lock(&reg_lock);
    char (*names)[256]; DWORD *types, *lens; uint8_t **dat; int n = load_all(dir, &names, &types, &dat, &lens); DWORD mn = 0, md = 0;
    for (int i = 0; i < n; i++) { DWORD l = (DWORD)strlen(names[i]); if (l > mn) mn = l; if (lens[i] > md) md = lens[i]; }
    free_all(n, names, types, dat, lens); pthread_mutex_unlock(&reg_lock);
    if (nsub) *nsub = ns; if (maxsub) *maxsub = ms; if (nval) *nval = (DWORD)n; if (maxname) *maxname = mn; if (maxdata) *maxdata = md;
    return 0;
}
WINAPI static LSTATUS e_RegGetValueA(HANDLE k, LPCSTR sub, LPCSTR name, DWORD flags, DWORD *type, void *data, DWORD *len)
{
    (void)flags; HANDLE h = k; int opened = 0;
    if (sub && *sub) { LSTATUS r = reg_open_a(k, sub, 0, &h, NULL); if (r) return r; opened = 1; }
    LSTATUS r = query_value(h, name, type, data, len, 0);
    if (opened) e_RegCloseKey(h);
    return r;
}
WINAPI static LSTATUS e_RegGetValueW(HANDLE k, LPCWSTR sub, LPCWSTR name, DWORD flags, DWORD *type, void *data, DWORD *len)
{
    (void)flags; char sa[600] = "", na[300] = ""; if (sub) w2u(sub, sa, sizeof sa); if (name) w2u(name, na, sizeof na);
    HANDLE h = k; int opened = 0;
    if (sa[0]) { LSTATUS r = reg_open_a(k, sa, 0, &h, NULL); if (r) return r; opened = 1; }
    LSTATUS r = query_value(h, na, type, data, len, 1);
    if (opened) e_RegCloseKey(h);
    return r;
}

/* ================================================================ Winsock (ws2_32): SOCKET = Linux fd */
#define WSA_INVALID ((SOCKET)~0ull)
static __thread int wsa_err;
static int wsa_map(int e)
{
    switch (e) {
    case EINTR: return 10004; case EBADF: return 10009; case EACCES: return 10013; case EFAULT: return 10014; case EINVAL: return 10022; case EMFILE: return 10024;
    case EAGAIN: return 10035; case EINPROGRESS: return 10036; case EALREADY: return 10037; case ENOTSOCK: return 10038; case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040; case EPROTOTYPE: return 10041; case ENOPROTOOPT: return 10042; case EPROTONOSUPPORT: return 10043; case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048; case EADDRNOTAVAIL: return 10049; case ENETDOWN: return 10050; case ENETUNREACH: return 10051; case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054; case ENOBUFS: return 10055; case EISCONN: return 10056; case ENOTCONN: return 10057; case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061; case EHOSTUNREACH: return 10065; case EPIPE: return 10053;
    }
    return 10000 + e;
}
static int wsa_fail(void) { wsa_err = wsa_map(errno); return -1; }
WINAPI static int e_WSAGetLastError(void) { return wsa_err; }
WINAPI static void e_WSASetLastError(int e) { wsa_err = e; }
WINAPI static int e_WSAStartup(WORD ver, uint8_t *data)
{
    memset(data, 0, 408);
    *(WORD *)data = ver; *(WORD *)(data + 2) = 0x0202; *(WORD *)(data + 4) = 32767; *(WORD *)(data + 6) = 65467;
    strcpy((char *)data + 16, "NEOS Winsock 2.2 over Linux sockets"); strcpy((char *)data + 16 + 257, "Running");
    return 0;
}
WINAPI static int e_WSACleanup(void) { return 0; }
WINAPI static SOCKET e_socket(int af, int type, int proto) { int fd = socket(af, type, proto); if (fd < 0) { wsa_fail(); return WSA_INVALID; } return (SOCKET)fd; }
WINAPI static SOCKET e_WSASocketA(int af, int type, int proto, void *info, DWORD grp, DWORD fl) { (void)info; (void)grp; (void)fl; return e_socket(af, type, proto); }
WINAPI static int e_closesocket(SOCKET s) { return close((int)s) ? wsa_fail() : 0; }
WINAPI static int e_connect(SOCKET s, const struct sockaddr *a, int n) { return connect((int)s, a, (socklen_t)n) ? wsa_fail() : 0; }
WINAPI static int e_bind(SOCKET s, const struct sockaddr *a, int n) { return bind((int)s, a, (socklen_t)n) ? wsa_fail() : 0; }
WINAPI static int e_listen(SOCKET s, int b) { return listen((int)s, b) ? wsa_fail() : 0; }
WINAPI static SOCKET e_accept(SOCKET s, struct sockaddr *a, int *n) { socklen_t l = n ? (socklen_t)*n : 0; int fd = accept((int)s, a, n ? &l : NULL); if (fd < 0) { wsa_fail(); return WSA_INVALID; } if (n) *n = (int)l; return (SOCKET)fd; }
WINAPI static int e_send(SOCKET s, const char *b, int n, int fl) { (void)fl; ssize_t r = send((int)s, b, (size_t)n, MSG_NOSIGNAL); return r < 0 ? wsa_fail() : (int)r; }
WINAPI static int e_recv(SOCKET s, char *b, int n, int fl) { ssize_t r = recv((int)s, b, (size_t)n, fl & 2 ? MSG_PEEK : 0); return r < 0 ? wsa_fail() : (int)r; }
WINAPI static int e_sendto(SOCKET s, const char *b, int n, int fl, const struct sockaddr *a, int al) { (void)fl; ssize_t r = sendto((int)s, b, (size_t)n, MSG_NOSIGNAL, a, (socklen_t)al); return r < 0 ? wsa_fail() : (int)r; }
WINAPI static int e_recvfrom(SOCKET s, char *b, int n, int fl, struct sockaddr *a, int *al) { socklen_t l = al ? (socklen_t)*al : 0; ssize_t r = recvfrom((int)s, b, (size_t)n, fl & 2 ? MSG_PEEK : 0, a, al ? &l : NULL); if (r < 0) return wsa_fail(); if (al) *al = (int)l; return (int)r; }
WINAPI static int e_shutdown(SOCKET s, int how) { return shutdown((int)s, how) ? wsa_fail() : 0; }
WINAPI static int e_getsockname(SOCKET s, struct sockaddr *a, int *n) { socklen_t l = (socklen_t)*n; int r = getsockname((int)s, a, &l); *n = (int)l; return r ? wsa_fail() : 0; }
WINAPI static int e_getpeername(SOCKET s, struct sockaddr *a, int *n) { socklen_t l = (socklen_t)*n; int r = getpeername((int)s, a, &l); *n = (int)l; return r ? wsa_fail() : 0; }
static int opt_level(int l) { return l == 0xFFFF ? SOL_SOCKET : l; }
static int opt_name(int level, int o)
{
    if (level != 0xFFFF) return o;
    switch (o) { case 4: return SO_REUSEADDR; case 8: return SO_KEEPALIVE; case 0x20: return SO_BROADCAST; case 0x1001: return SO_SNDBUF; case 0x1002: return SO_RCVBUF;
                 case 0x1005: return SO_SNDTIMEO; case 0x1006: return SO_RCVTIMEO; case 0x80: return SO_LINGER; case 0x1007: return SO_ERROR; case 0x1008: return SO_TYPE; }
    return o;
}
WINAPI static int e_setsockopt(SOCKET s, int level, int opt, const char *v, int n)
{
    int o = opt_name(level, opt);
    if (level == 0xFFFF && (opt == 0x1005 || opt == 0x1006) && n == 4) { DWORD ms = *(const DWORD *)v; struct timeval tv = { ms / 1000, (ms % 1000) * 1000 }; return setsockopt((int)s, SOL_SOCKET, o, &tv, sizeof tv) ? wsa_fail() : 0; }
    return setsockopt((int)s, opt_level(level), o, v, (socklen_t)n) ? wsa_fail() : 0;
}
WINAPI static int e_getsockopt(SOCKET s, int level, int opt, char *v, int *n)
{
    int o = opt_name(level, opt); socklen_t l = (socklen_t)*n;
    if (level == 0xFFFF && (opt == 0x1005 || opt == 0x1006) && *n >= 4) { struct timeval tv; socklen_t tl = sizeof tv; if (getsockopt((int)s, SOL_SOCKET, o, &tv, &tl)) return wsa_fail(); *(DWORD *)v = (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000); *n = 4; return 0; }
    int r = getsockopt((int)s, opt_level(level), o, v, &l); *n = (int)l; return r ? wsa_fail() : 0;
}
WINAPI static int e_ioctlsocket(SOCKET s, long cmd, unsigned long *arg)
{
    if ((unsigned long)cmd == 0x8004667Eul) { int fl = fcntl((int)s, F_GETFL); fcntl((int)s, F_SETFL, *arg ? fl | O_NONBLOCK : fl & ~O_NONBLOCK); return 0; }
    if ((unsigned long)cmd == 0x4004667Ful) { int n = 0; if (ioctl((int)s, FIONREAD, &n)) return wsa_fail(); *arg = (unsigned long)n; return 0; }
    wsa_err = 10022; return -1;
}
typedef struct { unsigned fd_count; SOCKET fd_array[64]; } WFdSet;
WINAPI static int e_select(int nfds, WFdSet *r, WFdSet *w, WFdSet *x, const struct timeval *tv)
{
    (void)nfds; struct pollfd pf[192]; int n = 0; WFdSet *sets[3] = { r, w, x }; int ev[3] = { POLLIN, POLLOUT, POLLPRI };
    int idx[192][2];
    for (int k = 0; k < 3; k++) if (sets[k]) for (unsigned i = 0; i < sets[k]->fd_count && n < 192; i++) { pf[n].fd = (int)sets[k]->fd_array[i]; pf[n].events = (short)ev[k]; pf[n].revents = 0; idx[n][0] = k; idx[n][1] = (int)i; n++; }
    int ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    int rc = poll(pf, (nfds_t)n, ms); if (rc < 0) return wsa_fail();
    WFdSet out[3]; memset(out, 0, sizeof out); int hits = 0;
    for (int i = 0; i < n; i++) {
        int k = idx[i][0]; short want = (short)ev[k];
        if (pf[i].revents & (want | POLLHUP | POLLERR)) { out[k].fd_array[out[k].fd_count++] = (SOCKET)pf[i].fd; hits++; }
    }
    for (int k = 0; k < 3; k++) if (sets[k]) *sets[k] = out[k];
    return hits;
}
typedef struct { SOCKET fd; short events, revents; } WPollFd;
WINAPI static int e_WSAPoll(WPollFd *f, unsigned long n, int ms)
{
    struct pollfd p[64]; if (n > 64) { wsa_err = 10022; return -1; }
    for (unsigned long i = 0; i < n; i++) { p[i].fd = (int)f[i].fd; p[i].events = (short)((f[i].events & 0x100 ? POLLIN : 0) | (f[i].events & 0x10 ? POLLOUT : 0)); p[i].revents = 0; }
    int rc = poll(p, (nfds_t)n, ms); if (rc < 0) return wsa_fail();
    for (unsigned long i = 0; i < n; i++) f[i].revents = (short)((p[i].revents & POLLIN ? 0x100 : 0) | (p[i].revents & POLLOUT ? 0x10 : 0) | (p[i].revents & POLLERR ? 1 : 0) | (p[i].revents & POLLHUP ? 2 : 0));
    return rc;
}
WINAPI static unsigned short e_htons(unsigned short v) { return htons(v); }
WINAPI static unsigned short e_ntohs(unsigned short v) { return ntohs(v); }
WINAPI static unsigned long e_htonl(unsigned long v) { return htonl((uint32_t)v); }
WINAPI static unsigned long e_ntohl(unsigned long v) { return ntohl((uint32_t)v); }
WINAPI static unsigned long e_inet_addr(const char *s) { struct in_addr a; return inet_aton(s, &a) ? a.s_addr : 0xFFFFFFFFul; }
WINAPI static char *e_inet_ntoa(struct in_addr a) { static __thread char b[20]; strlcpy(b, inet_ntoa(a), sizeof b); return b; }
WINAPI static int e_inet_pton(int af, const char *s, void *d) { return inet_pton(af, s, d); }
WINAPI static const char *e_inet_ntop(int af, const void *s, char *d, size_t n) { return inet_ntop(af, s, d, (socklen_t)n); }
WINAPI static int e_gethostname(char *b, int n) { return gethostname(b, (size_t)n) ? wsa_fail() : 0; }
typedef struct { char *h_name; char **h_aliases; short h_addrtype, h_length; char **h_addr_list; } WHostent;
WINAPI static WHostent *e_gethostbyname(const char *name)
{
    static __thread WHostent he; static __thread char nm[256], *aliases[1], *addrs[9]; static __thread uint8_t ab[8][4];
    struct addrinfo hint = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(name, NULL, &hint, &res) || !res) { wsa_err = 11001; return NULL; }
    int n = 0; for (struct addrinfo *a = res; a && n < 8; a = a->ai_next) { memcpy(ab[n], &((struct sockaddr_in *)a->ai_addr)->sin_addr, 4); addrs[n] = (char *)ab[n]; n++; }
    addrs[n] = NULL; aliases[0] = NULL; strlcpy(nm, name, sizeof nm);
    he.h_name = nm; he.h_aliases = aliases; he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = addrs;
    freeaddrinfo(res); return &he;
}
typedef struct WAddrInfo { int ai_flags, ai_family, ai_socktype, ai_protocol; size_t ai_addrlen; char *ai_canonname; struct sockaddr *ai_addr; struct WAddrInfo *ai_next; } WAddrInfo;
WINAPI static int e_getaddrinfo(const char *node, const char *svc, const WAddrInfo *hint, WAddrInfo **out)
{
    struct addrinfo h = { 0 }, *res = NULL;
    if (hint) { h.ai_flags = hint->ai_flags & 0x7; h.ai_family = hint->ai_family; h.ai_socktype = hint->ai_socktype; h.ai_protocol = hint->ai_protocol; }
    if (!h.ai_family || h.ai_family == AF_INET6) h.ai_family = AF_INET;
    int rc = getaddrinfo(node, svc, &h, &res);
    if (rc) { wsa_err = rc == EAI_NONAME ? 11001 : rc == EAI_SERVICE ? 10109 : 10060; return wsa_err; }
    WAddrInfo *head = NULL, **tail = &head;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        WAddrInfo *w = calloc(1, sizeof *w + sizeof(struct sockaddr_storage));
        w->ai_flags = a->ai_flags; w->ai_family = a->ai_family; w->ai_socktype = a->ai_socktype; w->ai_protocol = a->ai_protocol; w->ai_addrlen = a->ai_addrlen;
        w->ai_addr = (struct sockaddr *)(w + 1); memcpy(w->ai_addr, a->ai_addr, a->ai_addrlen);
        if (a->ai_canonname) w->ai_canonname = strdup(a->ai_canonname);
        *tail = w; tail = &w->ai_next;
    }
    freeaddrinfo(res); *out = head; return 0;
}
WINAPI static void e_freeaddrinfo(WAddrInfo *a) { while (a) { WAddrInfo *n = a->ai_next; free(a->ai_canonname); free(a); a = n; } }

/* ================================================================ SRW locks, condition variables, one-time init, FLS, WaitOnAddress */
/* SRWLOCK / CONDITION_VARIABLE / INIT_ONCE are one pointer wide: the real object is allocated on first use */
static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t *srw_of(void **p)
{
    if (!*p) { pthread_mutex_lock(&lazy_lock); if (!*p) { pthread_rwlock_t *l = malloc(sizeof *l); pthread_rwlock_init(l, NULL); *p = l; } pthread_mutex_unlock(&lazy_lock); }
    return *p;
}
WINAPI static void e_InitializeSRWLock(void **p) { *p = NULL; }
WINAPI static void e_AcquireSRWLockExclusive(void **p) { pthread_rwlock_wrlock(srw_of(p)); }
WINAPI static void e_ReleaseSRWLockExclusive(void **p) { pthread_rwlock_unlock(srw_of(p)); }
WINAPI static void e_AcquireSRWLockShared(void **p) { pthread_rwlock_rdlock(srw_of(p)); }
WINAPI static void e_ReleaseSRWLockShared(void **p) { pthread_rwlock_unlock(srw_of(p)); }
WINAPI static uint8_t e_TryAcquireSRWLockExclusive(void **p) { return pthread_rwlock_trywrlock(srw_of(p)) == 0; }
WINAPI static uint8_t e_TryAcquireSRWLockShared(void **p) { return pthread_rwlock_tryrdlock(srw_of(p)) == 0; }
typedef struct { pthread_cond_t c; pthread_mutex_t m; } CvImpl;
static CvImpl *cv_of(void **p)
{
    if (!*p) { pthread_mutex_lock(&lazy_lock); if (!*p) { CvImpl *c = malloc(sizeof *c); pthread_cond_init(&c->c, NULL); pthread_mutex_init(&c->m, NULL); *p = c; } pthread_mutex_unlock(&lazy_lock); }
    return *p;
}
WINAPI static void e_InitializeConditionVariable(void **p) { *p = NULL; }
WINAPI static void e_WakeConditionVariable(void **p) { CvImpl *c = cv_of(p); pthread_mutex_lock(&c->m); pthread_cond_signal(&c->c); pthread_mutex_unlock(&c->m); }
WINAPI static void e_WakeAllConditionVariable(void **p) { CvImpl *c = cv_of(p); pthread_mutex_lock(&c->m); pthread_cond_broadcast(&c->c); pthread_mutex_unlock(&c->m); }
static BOOL cv_wait(void **p, void (*unlock)(void *), void (*lock)(void *), void *lk, DWORD ms)
{
    CvImpl *c = cv_of(p);
    pthread_mutex_lock(&c->m); unlock(lk);                  /* the signal cannot slip between release and wait: it needs c->m too */
    int rc = 0;
    if (ms == 0xFFFFFFFFu) pthread_cond_wait(&c->c, &c->m);
    else { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; } rc = pthread_cond_timedwait(&c->c, &c->m, &ts); }
    pthread_mutex_unlock(&c->m); lock(lk);
    if (rc) { set_last_error(1460); return FALSE; }
    return TRUE;
}
static void cs_unlock(void *cs) { extern void *rt_resolve(const char *, const char *); WINAPI void (*f)(void *) = rt_resolve("", "LeaveCriticalSection"); f(cs); }
static void cs_lock(void *cs)   { extern void *rt_resolve(const char *, const char *); WINAPI void (*f)(void *) = rt_resolve("", "EnterCriticalSection"); f(cs); }
static void srw_unlock_x(void *l) { pthread_rwlock_unlock(srw_of(l)); }
static void srw_lock_x(void *l) { pthread_rwlock_wrlock(srw_of(l)); }
WINAPI static BOOL e_SleepConditionVariableCS(void **cv, void *cs, DWORD ms) { return cv_wait(cv, cs_unlock, cs_lock, cs, ms); }
WINAPI static BOOL e_SleepConditionVariableSRW(void **cv, void **srw, DWORD ms, ULONG flags)
{
    return cv_wait(cv, srw_unlock_x, srw_lock_x, srw, ms);
}
WINAPI static BOOL e_InitOnceExecuteOnce(void **once, BOOL (WINAPI *fn)(void *, void *, void **), void *param, void **ctx)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&m);
    BOOL ok = TRUE;
    if (*once != (void *)2) { void *c = NULL; ok = fn(once, param, &c); if (ok) *once = (void *)2; if (ctx) *ctx = c; }
    pthread_mutex_unlock(&m); return ok;
}
WINAPI static void e_InitOnceInitialize(void **once) { *once = NULL; }
static void *fls_slots[128]; static int fls_next = 1;
WINAPI static DWORD e_FlsAlloc(void *cb) { (void)cb; return fls_next < 128 ? (DWORD)fls_next++ : 0xFFFFFFFFu; }
WINAPI static BOOL e_FlsFree(DWORD i) { (void)i; return TRUE; }
WINAPI static void *e_FlsGetValue(DWORD i) { return i < 128 ? fls_slots[i] : NULL; }
WINAPI static BOOL e_FlsSetValue(DWORD i, void *v) { if (i >= 128) return FALSE; fls_slots[i] = v; return TRUE; }
WINAPI static BOOL e_WaitOnAddress(volatile void *addr, void *cmp, size_t sz, DWORD ms)
{
    uint64_t waited = 0;
    while (memcmp((const void *)addr, cmp, sz) == 0) { if (ms != 0xFFFFFFFFu && waited >= ms) { set_last_error(1460); return FALSE; } usleep(500); waited += 1; }
    return TRUE;
}
WINAPI static void e_WakeByAddressSingle(void *a) { (void)a; }
WINAPI static void e_WakeByAddressAll(void *a) { (void)a; }

/* interlocked variants not in kernel32.c */
WINAPI static LONG e_InterlockedAnd(volatile LONG *p, LONG v) { return __sync_fetch_and_and(p, v); }
WINAPI static LONG e_InterlockedOr(volatile LONG *p, LONG v) { return __sync_fetch_and_or(p, v); }
WINAPI static LONG e_InterlockedXor(volatile LONG *p, LONG v) { return __sync_fetch_and_xor(p, v); }
WINAPI static LONGLONG e_InterlockedIncrement64(volatile LONGLONG *p) { return __sync_add_and_fetch(p, 1); }
WINAPI static LONGLONG e_InterlockedDecrement64(volatile LONGLONG *p) { return __sync_sub_and_fetch(p, 1); }
WINAPI static LONGLONG e_InterlockedExchangeAdd64(volatile LONGLONG *p, LONGLONG v) { return __sync_fetch_and_add(p, v); }
WINAPI static LONGLONG e_InterlockedExchange64(volatile LONGLONG *p, LONGLONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
WINAPI static LONGLONG e_InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG x, LONGLONG c) { __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
WINAPI static void *e_InterlockedExchangePointer(void *volatile *p, void *v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
WINAPI static void *e_InterlockedCompareExchangePointer(void *volatile *p, void *x, void *c) { __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }

/* ================================================================ multimedia timers, scheduling hints, COM start-up, system queries */
static uint64_t mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
WINAPI static DWORD e_timeGetTime(void) { return (DWORD)mono_ms(); }
WINAPI static DWORD e_timeBeginPeriod(DWORD p) { (void)p; return 0; }
WINAPI static DWORD e_timeEndPeriod(DWORD p) { (void)p; return 0; }
WINAPI static BOOL e_SetPriorityClass(HANDLE p, DWORD c) { (void)p; (void)c; return TRUE; }
WINAPI static DWORD e_GetPriorityClass(HANDLE p) { (void)p; return 0x20; }
WINAPI static int e_GetThreadPriority(HANDLE t) { (void)t; return 0; }
WINAPI static ULONG_PTR e_SetThreadAffinityMask(HANDLE t, ULONG_PTR m) { (void)t; (void)m; return 1; }
WINAPI static BOOL e_SetProcessAffinityMask(HANDLE p, ULONG_PTR m) { (void)p; (void)m; return TRUE; }
WINAPI static BOOL e_GetProcessAffinityMask(HANDLE p, ULONG_PTR *pm, ULONG_PTR *sm) { (void)p; if (pm) *pm = 1; if (sm) *sm = 1; return TRUE; }
WINAPI static DWORD e_GetCurrentProcessorNumber(void) { return 0; }
WINAPI static DWORD e_SetThreadExecutionState(DWORD f) { (void)f; return 0x80000000u; }
WINAPI static BOOL e_SetProcessWorkingSetSize(HANDLE p, size_t a, size_t b) { (void)p; (void)a; (void)b; return TRUE; }
WINAPI static BOOL e_IsWow64Process(HANDLE p, BOOL *w) { (void)p; *w = FALSE; return TRUE; }
WINAPI static void e_GetNativeSystemInfo(uint8_t *si) { memset(si, 0, 48); *(WORD *)si = 9; *(DWORD *)(si + 4) = 4096; *(void **)(si + 8) = (void *)0x10000; *(void **)(si + 16) = (void *)0x7FFFFFFEFFFFull; *(ULONG_PTR *)(si + 24) = 1; *(DWORD *)(si + 32) = 1; *(DWORD *)(si + 36) = 8664; *(DWORD *)(si + 40) = 0x10000; }

/* ---- COM start-up: objects are not provided, so creation fails the way an unregistered class does ---- */
WINAPI static LONG e_CoInitialize(void *r) { (void)r; return 0; }
WINAPI static LONG e_CoInitializeEx(void *r, DWORD f) { (void)r; (void)f; return 0; }
WINAPI static void e_CoUninitialize(void) { }
WINAPI static LONG e_CoCreateInstance(const void *clsid, void *outer, DWORD ctx, const void *iid, void **out) { (void)clsid; (void)outer; (void)ctx; (void)iid; if (out) *out = NULL; return (LONG)0x80040154; /* REGDB_E_CLASSNOTREG */ }
WINAPI static LONG e_CoInitializeSecurity(void *a, LONG b, void *c, void *d, DWORD e, DWORD f, void *g, DWORD h, void *i) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; return 0; }
WINAPI static void *e_CoTaskMemAlloc(size_t n) { return malloc(n); }
WINAPI static void e_CoTaskMemFree(void *p) { free(p); }
WINAPI static WCHAR *e_SysAllocString(const WCHAR *s)
{
    if (!s) return NULL; size_t n = 0; while (s[n]) n++;
    uint8_t *m = malloc(4 + (n + 1) * 2); *(uint32_t *)m = (uint32_t)(n * 2); memcpy(m + 4, s, (n + 1) * 2); return (WCHAR *)(m + 4);
}
WINAPI static void e_SysFreeString(WCHAR *s) { if (s) free((uint8_t *)s - 4); }

/* ---- system directory / identity queries ---- */
static DWORD put_a(const char *v, LPSTR out, DWORD n) { DWORD l = (DWORD)strlen(v); if (l + 1 > n) return l + 1; memcpy(out, v, l + 1); return l; }
static DWORD put_w(const char *v, LPWSTR out, DWORD n) { DWORD l = (DWORD)strlen(v); if (l + 1 > n) return l + 1; u2w(v, out, n); return l; }
WINAPI static DWORD e_GetSystemDirectoryA(LPSTR b, DWORD n) { return put_a("C:\\Windows\\System32", b, n); }
WINAPI static DWORD e_GetSystemDirectoryW(LPWSTR b, DWORD n) { return put_w("C:\\Windows\\System32", b, n); }
WINAPI static DWORD e_GetWindowsDirectoryA(LPSTR b, DWORD n) { return put_a("C:\\Windows", b, n); }
WINAPI static DWORD e_GetWindowsDirectoryW(LPWSTR b, DWORD n) { return put_w("C:\\Windows", b, n); }
WINAPI static DWORD e_GetSystemWindowsDirectoryA(LPSTR b, DWORD n) { return put_a("C:\\Windows", b, n); }
WINAPI static BOOL e_GetComputerNameW(LPWSTR b, DWORD *n) { if (*n < 5) { *n = 5; return FALSE; } u2w("NEOS", b, *n); *n = 4; return TRUE; }
WINAPI static BOOL e_GetUserNameW(LPWSTR b, DWORD *n) { if (*n < 5) { *n = 5; return FALSE; } u2w("neos", b, *n); *n = 5; return TRUE; }
WINAPI static BOOL e_GetComputerNameExA(int t, LPSTR b, DWORD *n) { (void)t; if (*n < 5) { *n = 5; return FALSE; } strcpy(b, "NEOS"); *n = 4; return TRUE; }
WINAPI static DWORD e_GetTempPathW(DWORD n, LPWSTR b) { return put_w("C:\\Temp\\", b, n); }
WINAPI static BOOL e_SHGetSpecialFolderPathA(void *w, LPSTR b, int csidl, BOOL c) { (void)w; (void)c; const char *p = csidl == 0x26 || csidl == 0x2a ? "C:\\Program Files" : csidl == 0x1c || csidl == 0x23 || csidl == 0x1a ? "C:\\ProgramData" : "C:\\Users\\neos"; strcpy(b, p); return TRUE; }

/* ================================================================ table */
typedef struct { const char *name; void *fn; } ESym;
#define X(n) { #n, (void *)e_##n }
static const ESym esyms[] = {
    X(RegOpenKeyExA), X(RegOpenKeyExW), X(RegOpenKeyA), X(RegCreateKeyExA), X(RegCreateKeyExW), X(RegCreateKeyA), X(RegCloseKey), X(RegFlushKey),
    X(RegQueryValueExA), X(RegQueryValueExW), X(RegQueryValueA), X(RegSetValueExA), X(RegSetValueExW), X(RegSetValueA), X(RegDeleteValueA), X(RegDeleteValueW),
    X(RegDeleteKeyA), X(RegDeleteKeyW), X(RegDeleteTreeA), X(RegEnumKeyExA), X(RegEnumKeyExW), X(RegEnumKeyA), X(RegEnumValueA), X(RegEnumValueW),
    X(RegQueryInfoKeyA), X(RegGetValueA), X(RegGetValueW),
    X(WSAGetLastError), X(WSASetLastError), X(WSAStartup), X(WSACleanup), X(socket), X(WSASocketA), X(closesocket), X(connect), X(bind), X(listen), X(accept),
    X(send), X(recv), X(sendto), X(recvfrom), X(shutdown), X(getsockname), X(getpeername), X(setsockopt), X(getsockopt), X(ioctlsocket), X(select), X(WSAPoll),
    X(htons), X(ntohs), X(htonl), X(ntohl), X(inet_addr), X(inet_ntoa), X(inet_pton), X(inet_ntop), X(gethostname), X(gethostbyname), X(getaddrinfo), X(freeaddrinfo),
    X(InitializeSRWLock), X(AcquireSRWLockExclusive), X(ReleaseSRWLockExclusive), X(AcquireSRWLockShared), X(ReleaseSRWLockShared), X(TryAcquireSRWLockExclusive),
    X(TryAcquireSRWLockShared), X(InitializeConditionVariable), X(WakeConditionVariable), X(WakeAllConditionVariable), X(SleepConditionVariableCS),
    X(SleepConditionVariableSRW), X(InitOnceExecuteOnce), X(InitOnceInitialize), X(FlsAlloc), X(FlsFree), X(FlsGetValue), X(FlsSetValue), X(WaitOnAddress),
    X(WakeByAddressSingle), X(WakeByAddressAll),
    X(InterlockedAnd), X(InterlockedOr), X(InterlockedXor), X(InterlockedIncrement64), X(InterlockedDecrement64), X(InterlockedExchangeAdd64), X(InterlockedExchange64),
    X(InterlockedCompareExchange64), X(InterlockedExchangePointer), X(InterlockedCompareExchangePointer),
    X(timeGetTime), X(timeBeginPeriod), X(timeEndPeriod), X(SetPriorityClass), X(GetPriorityClass), X(GetThreadPriority), X(SetThreadAffinityMask), X(SetProcessAffinityMask),
    X(GetProcessAffinityMask), X(GetCurrentProcessorNumber), X(SetThreadExecutionState), X(SetProcessWorkingSetSize), X(IsWow64Process), X(GetNativeSystemInfo),
    X(CoInitialize), X(CoInitializeEx), X(CoUninitialize), X(CoCreateInstance), X(CoInitializeSecurity), X(CoTaskMemAlloc), X(CoTaskMemFree), X(SysAllocString), X(SysFreeString),
    X(GetSystemDirectoryA), X(GetSystemDirectoryW), X(GetWindowsDirectoryA), X(GetWindowsDirectoryW), X(GetSystemWindowsDirectoryA), X(GetComputerNameW), X(GetUserNameW),
    X(GetComputerNameExA), X(GetTempPathW), X(SHGetSpecialFolderPathA),
    { NULL, NULL }
};
void *ext_resolve(const char *name) { for (const ESym *s = esyms; s->name; s++) if (!strcmp(s->name, name)) return s->fn; return NULL; }
void ext_list(void (*cb)(const char *)) { for (const ESym *s = esyms; s->name; s++) cb(s->name); }
