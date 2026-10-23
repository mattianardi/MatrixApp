/* kernel32 / ntdll shim: handles, files, threads, sync, memory, process, console, time. Everything is built on Linux-ABI calls. */
#include "winrt.h"
#include <sys/time.h>
#include <fnmatch.h>
#include <sched.h>
#include <malloc.h>
#include <sys/stat.h>

char g_exe_path_win[260], g_exe_path_lin[260];
int g_argc; char **g_argv;
static int g_argi = 2;                                   /* first program argument in g_argv */

/* ------------------------------------------------------------ TEB / PEB */
#define TEB_SIZE 0x1000
static uint8_t peb[0x400];
static pthread_key_t dummy_key;

void *teb_tls_slot(void) { uint64_t t; __asm__ volatile("movq %%gs:0x30,%0" : "=r"(t)); return (void *)(t + 0x58); }
DWORD *teb_last_error(void) { uint64_t t; __asm__ volatile("movq %%gs:0x30,%0" : "=r"(t)); return (DWORD *)(t + 0x68); }
void set_last_error(DWORD e) { *teb_last_error() = e; }
void pe_setup_tls_for_thread(void **tls_array);

void teb_init_thread(void *stack_base)
{
    uint8_t *t = calloc(1, TEB_SIZE);
    void **tls = calloc(64, sizeof(void *));
    uint64_t sp = (uint64_t)__builtin_frame_address(0);
    *(uint64_t *)(t + 0x00) = 0;                                   /* exception list */
    *(uint64_t *)(t + 0x08) = (uint64_t)(stack_base ? stack_base : (void *)((sp | 0xFFFFF) + 1));
    *(uint64_t *)(t + 0x10) = (uint64_t)(stack_base ? stack_base : (void *)(sp - 0x800000));
    *(uint64_t *)(t + 0x30) = (uint64_t)t;
    *(uint64_t *)(t + 0x40) = (uint64_t)getpid();
    *(uint64_t *)(t + 0x48) = (uint64_t)syscall(SYS_gettid);
    *(uint64_t *)(t + 0x58) = (uint64_t)tls;
    *(uint64_t *)(t + 0x60) = (uint64_t)peb;
    syscall(SYS_arch_prctl, 0x1001, t);                            /* GS base = TEB */
    pe_setup_tls_for_thread(tls);
}

DWORD errno_to_win(int e)
{
    switch (e) {
    case 0: return 0;
    case ENOENT: return 2; case ENOTDIR: return 3; case EMFILE: return 4; case EACCES: case EPERM: case EROFS: return 5;
    case EBADF: return 6; case ENOMEM: return 8; case EEXIST: return 80; case EINVAL: return 87; case ENOTEMPTY: return 145;
    case EISDIR: return 5; case ENOSPC: return 112; case EPIPE: return 109; case EAGAIN: return 258; case ESRCH: return 87;
    default: return 31;
    }
}
static BOOL fail_errno(void) { set_last_error(errno_to_win(errno)); return FALSE; }

/* ------------------------------------------------------------ handle table */
enum { O_NONE, O_FILE, O_THREAD, O_EVENT, O_MUTEX, O_SEM, O_PROC, O_FIND };
typedef struct Obj {
    int type, fd;
    int manual, signaled, count, max;                  /* event / semaphore */
    pthread_t th; int started, done, suspended; DWORD exitcode; void *fn, *arg; DWORD tid;
    pid_t pid;
    DIR *dir; char pat[260], dirpath[300];
    DWORD owner; int depth;                            /* mutex */
} Obj;
#define MAXH 1024
static Obj *htab[MAXH];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static HANDLE h_new(Obj *o)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAXH; i++) if (!htab[i]) { htab[i] = o; pthread_mutex_unlock(&g_lock); return (HANDLE)(intptr_t)((i + 1) * 4); }
    pthread_mutex_unlock(&g_lock);
    free(o); set_last_error(4); return INVALID_HANDLE_VALUE;
}
static Obj *H(HANDLE h)
{
    intptr_t v = (intptr_t)h;
    if (v <= 0 || (v & 3) || v / 4 > MAXH) return NULL;
    return htab[v / 4 - 1];
}
static HANDLE file_handle(int fd) { Obj *o = calloc(1, sizeof *o); o->type = O_FILE; o->fd = fd; return h_new(o); }
static void lock(void) { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_lock); }

static HANDLE std_h[3];
static void std_init(void) { for (int i = 0; i < 3; i++) std_h[i] = file_handle(i); }

static DWORD tid_self(void) { return (DWORD)syscall(SYS_gettid); }

/* ------------------------------------------------------------ small helpers */
static char *wide_to_utf8(const WCHAR *w, int n, char *out, size_t cap)
{
    size_t l = 0;
    for (int i = 0; (n < 0 ? w[i] : i < n); i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1] >= 0xDC00) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        char b[4]; int k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); k = 3; }
        else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); k = 4; }
        if (l + (size_t)k + 1 > cap) break;
        memcpy(out + l, b, (size_t)k); l += (size_t)k;
    }
    out[l] = 0; return out;
}
static int utf8_to_wide(const char *s, int n, WCHAR *out, int cap)
{
    int l = 0; const unsigned char *p = (const unsigned char *)s, *e = n < 0 ? (const unsigned char *)s + strlen(s) + 1 : p + n;
    while (p < e) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p + 2 < e + 0) { c = (c & 7) << 18 | (uint32_t)(p[0] & 63) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p + 1 < e + 0) { c = (c & 15) << 12 | (uint32_t)(p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p < e) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        if (c >= 0x10000) { if (out && l + 2 <= cap) { c -= 0x10000; out[l] = (WCHAR)(0xD800 + (c >> 10)); out[l + 1] = (WCHAR)(0xDC00 + (c & 0x3FF)); } l += 2; }
        else { if (out && l < cap) out[l] = (WCHAR)c; l++; }
    }
    return l;
}
static char *lp(const char *w, char *buf) { return path_win_to_lin(w, buf, 300); }
#define LP(name, w) char name[300]; lp((w), name)
#define LPW(name, w) char name##_a[300], name[300]; wide_to_utf8((w), -1, name##_a, sizeof name##_a); lp(name##_a, name)

/* ------------------------------------------------------------ errors, process basics */
WINAPI static DWORD k_GetLastError(void) { return *teb_last_error(); }
WINAPI static void k_SetLastError(DWORD e) { set_last_error(e); }
extern void crt_flush_all(void);
WINAPI static void k_ExitProcess(DWORD c) { crt_flush_all(); _exit((int)c); }
WINAPI static BOOL k_TerminateProcess(HANDLE h, DWORD c) { Obj *o = H(h); if (h == (HANDLE)(intptr_t)-1 || !o) k_ExitProcess(c); if (o->type == O_PROC) kill(o->pid, SIGKILL); return TRUE; }
WINAPI static HANDLE k_GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
WINAPI static HANDLE k_GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
WINAPI static DWORD k_GetCurrentProcessId(void) { return (DWORD)getpid(); }
WINAPI static DWORD k_GetCurrentThreadId(void) { return tid_self(); }
WINAPI static void k_Sleep(DWORD ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
WINAPI static BOOL k_SwitchToThread(void) { sched_yield(); return TRUE; }

/* ------------------------------------------------------------ memory */
WINAPI static HANDLE k_GetProcessHeap(void) { return (HANDLE)0x1000; }
WINAPI static HANDLE k_HeapCreate(DWORD f, size_t a, size_t b) { (void)f; (void)a; (void)b; return (HANDLE)0x1000; }
WINAPI static BOOL k_HeapDestroy(HANDLE h) { (void)h; return TRUE; }
WINAPI static LPVOID k_HeapAlloc(HANDLE h, DWORD f, size_t n) { (void)h; return (f & 8) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
WINAPI static LPVOID k_HeapReAlloc(HANDLE h, DWORD f, LPVOID p, size_t n) { (void)h; (void)f; return realloc(p, n ? n : 1); }
WINAPI static BOOL k_HeapFree(HANDLE h, DWORD f, LPVOID p) { (void)h; (void)f; free(p); return TRUE; }
WINAPI static size_t k_HeapSize(HANDLE h, DWORD f, LPCVOID p) { (void)h; (void)f; return p ? malloc_usable_size((void *)p) : (size_t)-1; }
WINAPI static LPVOID k_LocalAlloc(DWORD f, size_t n) { return (f & 0x40) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
WINAPI static LPVOID k_LocalFree(LPVOID p) { free(p); return NULL; }
WINAPI static LPVOID k_GlobalAlloc(DWORD f, size_t n) { return (f & 0x40) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
WINAPI static LPVOID k_GlobalFree(LPVOID p) { free(p); return NULL; }
static int vprot(DWORD p)
{
    switch (p & 0xFF) {
    case 0x01: return PROT_NONE; case 0x02: return PROT_READ; case 0x04: case 0x08: return PROT_READ | PROT_WRITE;
    case 0x10: return PROT_EXEC; case 0x20: return PROT_READ | PROT_EXEC; case 0x40: case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}
WINAPI static LPVOID k_VirtualAlloc(LPVOID a, size_t n, DWORD type, DWORD prot)
{
    if (!(type & 0x3000)) { set_last_error(87); return NULL; }
    if (a && !(type & 0x2000)) { mprotect(a, n, vprot(prot)); return a; }       /* commit inside an earlier reservation */
    void *p = mmap(a, n, vprot(prot), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { set_last_error(8); return NULL; }
    return p;
}
WINAPI static BOOL k_VirtualFree(LPVOID a, size_t n, DWORD type) { if (type & 0x8000) { if (n) munmap(a, n); } return TRUE; }
WINAPI static BOOL k_VirtualProtect(LPVOID a, size_t n, DWORD np, LPDWORD old)
{
    if (old) *old = 0x04;
    uintptr_t s = (uintptr_t)a & ~0xFFFul, e = ((uintptr_t)a + n + 0xFFF) & ~0xFFFul;
    return mprotect((void *)s, e - s, vprot(np)) ? fail_errno() : TRUE;
}
WINAPI static size_t k_VirtualQuery(LPCVOID a, void *mbi, size_t len)
{
    if (len < 48) return 0;
    memset(mbi, 0, 48);
    uintptr_t b = (uintptr_t)a & ~0xFFFul;
    *(uint64_t *)((char *)mbi + 0) = b; *(uint64_t *)((char *)mbi + 8) = b;
    *(DWORD *)((char *)mbi + 16) = 4; *(uint64_t *)((char *)mbi + 24) = 0x1000;
    *(DWORD *)((char *)mbi + 32) = 0x1000; *(DWORD *)((char *)mbi + 36) = 4; *(DWORD *)((char *)mbi + 40) = 0x20000;
    return 48;
}
WINAPI static void k_GetSystemInfo(void *si)
{
    memset(si, 0, 48);
    *(WORD *)si = 9; *(DWORD *)((char *)si + 4) = 4096; *(uint64_t *)((char *)si + 8) = 0x10000;
    *(uint64_t *)((char *)si + 16) = 0x7FFFFFFEFFFFull; *(uint64_t *)((char *)si + 24) = 1; *(DWORD *)((char *)si + 32) = 1;
    *(DWORD *)((char *)si + 36) = 8664; *(DWORD *)((char *)si + 40) = 0x10000;
}

/* ------------------------------------------------------------ files */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
static HANDLE open_file(const char *path, DWORD acc, DWORD disp, DWORD attrs)
{
    int fl = (acc & GENERIC_WRITE) ? ((acc & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    struct stat st; int ex = stat(path, &st) == 0;
    switch (disp) {
    case 1: if (ex) { set_last_error(80); return INVALID_HANDLE_VALUE; } fl |= O_CREAT | O_EXCL; break;
    case 2: fl |= O_CREAT | O_TRUNC; break;
    case 3: if (!ex) { set_last_error(2); return INVALID_HANDLE_VALUE; } break;
    case 4: fl |= O_CREAT; break;
    case 5: if (!ex) { set_last_error(2); return INVALID_HANDLE_VALUE; } fl |= O_TRUNC; break;
    default: set_last_error(87); return INVALID_HANDLE_VALUE;
    }
    if (ex && S_ISDIR(st.st_mode)) fl = O_RDONLY;
    (void)attrs;
    int fd = open(path, fl, 0644);
    if (fd < 0) { fail_errno(); return INVALID_HANDLE_VALUE; }
    set_last_error(ex && (disp == 2 || disp == 4) ? 183 : 0);
    return file_handle(fd);
}
WINAPI static HANDLE k_CreateFileA(LPCSTR n, DWORD acc, DWORD sh, LPVOID sa, DWORD disp, DWORD fl, HANDLE t) { (void)sh; (void)sa; (void)t; LP(p, n); return open_file(p, acc, disp, fl); }
WINAPI static HANDLE k_CreateFileW(LPCWSTR n, DWORD acc, DWORD sh, LPVOID sa, DWORD disp, DWORD fl, HANDLE t) { (void)sh; (void)sa; (void)t; LPW(p, n); return open_file(p, acc, disp, fl); }
WINAPI static BOOL k_CloseHandle(HANDLE h)
{
    Obj *o = H(h);
    if (!o) { set_last_error(6); return FALSE; }
    if (o->type == O_FILE && o->fd > 2) close(o->fd);
    if (o->type == O_FIND && o->dir) closedir(o->dir);
    if (o->type == O_FILE && o->fd <= 2) return TRUE;
    lock(); htab[(intptr_t)h / 4 - 1] = NULL; unlock();
    if (o->type != O_THREAD) free(o);
    return TRUE;
}
WINAPI static BOOL k_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPVOID ov)
{
    Obj *o = H(h); (void)ov;
    if (!o || o->type != O_FILE) { set_last_error(6); return FALSE; }
    if (o->fd == 0) crt_flush_all();
    ssize_t r = read(o->fd, buf, n);
    if (r < 0) { if (got) *got = 0; return fail_errno(); }
    if (got) *got = (DWORD)r;
    return TRUE;
}
WINAPI static BOOL k_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPVOID ov)
{
    Obj *o = H(h); (void)ov;
    if (!o || o->type != O_FILE) { set_last_error(6); return FALSE; }
    if (o->fd == 1 || o->fd == 2) crt_flush_all();
    DWORD done = 0;
    while (done < n) { ssize_t r = write(o->fd, (const char *)buf + done, n - done); if (r < 0) { if (put) *put = done; return fail_errno(); } done += (DWORD)r; }
    if (put) *put = done;
    return TRUE;
}
WINAPI static DWORD k_SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD how)
{
    Obj *o = H(h); if (!o || o->type != O_FILE) { set_last_error(6); return 0xFFFFFFFF; }
    off_t off = hi ? ((off_t)*hi << 32) | (uint32_t)lo : (off_t)lo;
    off_t r = lseek(o->fd, off, (int)how);
    if (r < 0) { fail_errno(); return 0xFFFFFFFF; }
    if (hi) *hi = (LONG)(r >> 32);
    return (DWORD)r;
}
WINAPI static BOOL k_SetFilePointerEx(HANDLE h, LONGLONG off, LONGLONG *np, DWORD how)
{
    Obj *o = H(h); if (!o || o->type != O_FILE) { set_last_error(6); return FALSE; }
    off_t r = lseek(o->fd, (off_t)off, (int)how);
    if (r < 0) return fail_errno();
    if (np) *np = r;
    return TRUE;
}
WINAPI static BOOL k_GetFileSizeEx(HANDLE h, LONGLONG *sz) { Obj *o = H(h); struct stat st; if (!o || o->type != O_FILE || fstat(o->fd, &st)) { set_last_error(6); return FALSE; } *sz = st.st_size; return TRUE; }
WINAPI static DWORD k_GetFileSize(HANDLE h, LPDWORD hi) { LONGLONG s; if (!k_GetFileSizeEx(h, &s)) return 0xFFFFFFFF; if (hi) *hi = (DWORD)(s >> 32); return (DWORD)s; }
WINAPI static BOOL k_FlushFileBuffers(HANDLE h) { (void)h; return TRUE; }
WINAPI static BOOL k_SetEndOfFile(HANDLE h) { Obj *o = H(h); if (!o) return FALSE; off_t p = lseek(o->fd, 0, SEEK_CUR); return ftruncate(o->fd, p) ? fail_errno() : TRUE; }
WINAPI static DWORD k_GetFileType(HANDLE h)
{
    Obj *o = H(h); struct stat st; if (!o || o->type != O_FILE) return 0;
    if (isatty(o->fd)) return 2;
    if (!fstat(o->fd, &st) && S_ISFIFO(st.st_mode)) return 3;
    return 1;
}
WINAPI static BOOL k_DeleteFileA(LPCSTR n) { LP(p, n); return unlink(p) ? fail_errno() : TRUE; }
WINAPI static BOOL k_DeleteFileW(LPCWSTR n) { LPW(p, n); return unlink(p) ? fail_errno() : TRUE; }
WINAPI static BOOL k_CreateDirectoryA(LPCSTR n, LPVOID sa) { (void)sa; LP(p, n); if (mkdir(p, 0755)) { fail_errno(); if (errno == EEXIST) set_last_error(183); return FALSE; } return TRUE; }
WINAPI static BOOL k_CreateDirectoryW(LPCWSTR n, LPVOID sa) { (void)sa; LPW(p, n); if (mkdir(p, 0755)) { fail_errno(); if (errno == EEXIST) set_last_error(183); return FALSE; } return TRUE; }
WINAPI static BOOL k_RemoveDirectoryA(LPCSTR n) { LP(p, n); return rmdir(p) ? fail_errno() : TRUE; }
WINAPI static BOOL k_MoveFileA(LPCSTR a, LPCSTR b) { LP(p, a); LP(q, b); return rename(p, q) ? fail_errno() : TRUE; }
WINAPI static BOOL k_MoveFileExA(LPCSTR a, LPCSTR b, DWORD f) { (void)f; return k_MoveFileA(a, b); }
WINAPI static BOOL k_CopyFileA(LPCSTR a, LPCSTR b, BOOL failexist)
{
    LP(p, a); LP(q, b); struct stat st;
    if (failexist && !stat(q, &st)) { set_last_error(80); return FALSE; }
    int in = open(p, O_RDONLY); if (in < 0) return fail_errno();
    int out = open(q, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return fail_errno(); }
    char buf[4096]; ssize_t r; while ((r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)r) != r) break;
    close(in); close(out); return TRUE;
}
static DWORD attrs_of(const char *p)
{
    struct stat st;
    if (stat(p, &st)) { set_last_error(errno == ENOENT ? 2 : errno_to_win(errno)); return 0xFFFFFFFF; }
    DWORD a = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    if (!(st.st_mode & 0222)) a |= 1;
    return a;
}
WINAPI static DWORD k_GetFileAttributesA(LPCSTR n) { LP(p, n); return attrs_of(p); }
WINAPI static DWORD k_GetFileAttributesW(LPCWSTR n) { LPW(p, n); return attrs_of(p); }
WINAPI static DWORD k_GetCurrentDirectoryA(DWORD n, LPSTR out)
{
    char l[300], w[300]; if (!getcwd(l, sizeof l)) return 0;
    path_lin_to_win(l, w, sizeof w);
    DWORD len = (DWORD)strlen(w);
    if (len + 1 > n) return len + 1;
    memcpy(out, w, len + 1); return len;
}
WINAPI static BOOL k_SetCurrentDirectoryA(LPCSTR n) { LP(p, n); return chdir(p) ? fail_errno() : TRUE; }
WINAPI static DWORD k_GetFullPathNameA(LPCSTR n, DWORD sz, LPSTR out, LPSTR *fp)
{
    char full[300];
    if (n[0] == '\\' || (isalpha((unsigned char)n[0]) && n[1] == ':')) strlcpy(full, n, sizeof full);
    else { char cw[300]; k_GetCurrentDirectoryA(sizeof cw, cw); snprintf(full, sizeof full, "%s\\%s", cw, n); }
    DWORD len = (DWORD)strlen(full);
    if (len + 1 > sz) return len + 1;
    memcpy(out, full, len + 1);
    if (fp) { char *s = strrchr(out, '\\'); *fp = s ? s + 1 : out; }
    return len;
}
WINAPI static DWORD k_GetTempPathA(DWORD n, LPSTR out) { const char *t = "C:\\Temp\\"; mkdir("/mnt/c/Temp", 0777); if (n < 10) return 10; strcpy(out, t); return 8; }

/* ------------------------------------------------------------ directory search */
static void fill_find(Obj *o, struct dirent *de, char *fd)
{
    char full[600]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", o->dirpath, de->d_name);
    memset(fd, 0, 320);
    if (stat(full, &st)) memset(&st, 0, sizeof st);
    *(DWORD *)fd = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    memcpy(fd + 4, &ft, 8); memcpy(fd + 12, &ft, 8); memcpy(fd + 20, &ft, 8);
    *(DWORD *)(fd + 28) = (DWORD)((uint64_t)st.st_size >> 32); *(DWORD *)(fd + 32) = (DWORD)st.st_size;
    strlcpy(fd + 44, de->d_name, 260);
}
static int find_next(Obj *o, char *fd)
{
    struct dirent *de;
    while ((de = readdir(o->dir))) if (!fnmatch(o->pat, de->d_name, FNM_CASEFOLD)) { fill_find(o, de, fd); return 1; }
    return 0;
}
WINAPI static HANDLE k_FindFirstFileA(LPCSTR n, LPVOID fd)
{
    LP(p, n); char *sl = strrchr(p, '/');
    Obj *o = calloc(1, sizeof *o); o->type = O_FIND;
    if (sl) { *sl = 0; strlcpy(o->dirpath, p[0] ? p : "/", sizeof o->dirpath); strlcpy(o->pat, sl + 1, sizeof o->pat); }
    else { strcpy(o->dirpath, "."); strlcpy(o->pat, p, sizeof o->pat); }
    if (!strcmp(o->pat, "*.*")) strcpy(o->pat, "*");
    o->dir = opendir(o->dirpath);
    if (!o->dir) { free(o); set_last_error(3); return INVALID_HANDLE_VALUE; }
    if (!find_next(o, fd)) { closedir(o->dir); free(o); set_last_error(2); return INVALID_HANDLE_VALUE; }
    return h_new(o);
}
WINAPI static BOOL k_FindNextFileA(HANDLE h, LPVOID fd) { Obj *o = H(h); if (!o || o->type != O_FIND) return FALSE; if (find_next(o, fd)) return TRUE; set_last_error(18); return FALSE; }
WINAPI static BOOL k_FindClose(HANDLE h) { return k_CloseHandle(h); }

/* ------------------------------------------------------------ console */
static DWORD con_mode = 3, con_attr = 7;
WINAPI static HANDLE k_GetStdHandle(DWORD id) { int i = id == (DWORD)-10 ? 0 : id == (DWORD)-11 ? 1 : id == (DWORD)-12 ? 2 : -1; if (i < 0) { set_last_error(87); return INVALID_HANDLE_VALUE; } return std_h[i]; }
WINAPI static BOOL k_SetStdHandle(DWORD id, HANDLE h) { int i = id == (DWORD)-10 ? 0 : id == (DWORD)-11 ? 1 : id == (DWORD)-12 ? 2 : -1; if (i < 0) return FALSE; std_h[i] = h; return TRUE; }
WINAPI static BOOL k_GetConsoleMode(HANDLE h, LPDWORD m) { Obj *o = H(h); if (!o || o->type != O_FILE || !isatty(o->fd)) { set_last_error(6); return FALSE; } *m = con_mode; return TRUE; }
WINAPI static BOOL k_SetConsoleMode(HANDLE h, DWORD m) { (void)h; con_mode = m; return TRUE; }
WINAPI static BOOL k_WriteConsoleA(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, LPVOID r) { return k_WriteFile(h, b, n, w, r); }
WINAPI static BOOL k_WriteConsoleW(HANDLE h, LPCWSTR b, DWORD n, LPDWORD w, LPVOID r)
{
    (void)r; char *u = malloc((size_t)n * 4 + 1); wide_to_utf8(b, (int)n, u, (size_t)n * 4 + 1);
    DWORD put; BOOL ok = k_WriteFile(h, u, (DWORD)strlen(u), &put, NULL); free(u);
    if (w) *w = n;
    return ok;
}
WINAPI static BOOL k_ReadConsoleA(HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPVOID r) { return k_ReadFile(h, b, n, got, r); }
WINAPI static BOOL k_SetConsoleTextAttribute(HANDLE h, WORD a)
{
    Obj *o = H(h); con_attr = a;
    if (o && o->type == O_FILE && isatty(o->fd)) {
        static const int ansi[8] = { 30, 34, 32, 36, 31, 35, 33, 37 };
        char s[32]; int fg = ansi[a & 7] + ((a & 8) ? 60 : 0), bg = ansi[(a >> 4) & 7] + 10 + ((a & 0x80) ? 60 : 0);
        int n = snprintf(s, sizeof s, "\x1b[0;%d;%dm", fg, bg);
        crt_flush_all(); if (write(o->fd, s, (size_t)n) < 0) {}
    }
    return TRUE;
}
WINAPI static BOOL k_SetConsoleTitleA(LPCSTR t) { (void)t; return TRUE; }
WINAPI static BOOL k_SetConsoleCtrlHandler(LPVOID f, BOOL a) { (void)f; (void)a; return TRUE; }
WINAPI static BOOL k_GetConsoleScreenBufferInfo(HANDLE h, void *info)
{
    (void)h; memset(info, 0, 22);
    ((WORD *)info)[0] = 80; ((WORD *)info)[1] = 25; ((WORD *)info)[4] = (WORD)con_attr; ((WORD *)info)[5] = 0; ((WORD *)info)[6] = 0;
    ((WORD *)info)[7] = 79; ((WORD *)info)[8] = 24; ((WORD *)info)[9] = 80; ((WORD *)info)[10] = 25;
    return TRUE;
}
WINAPI static DWORD k_GetConsoleCP(void) { return 65001; }
WINAPI static BOOL k_SetConsoleCP(DWORD c) { (void)c; return TRUE; }
WINAPI static void k_OutputDebugStringA(LPCSTR s) { fputs(s, stderr); }
WINAPI static BOOL k_CreatePipe(HANDLE *r, HANDLE *w, LPVOID sa, DWORD sz)
{
    (void)sa; (void)sz; int fds[2];
    if (pipe(fds)) return fail_errno();
    *r = file_handle(fds[0]); *w = file_handle(fds[1]);
    return TRUE;
}
WINAPI static BOOL k_PeekNamedPipe(HANDLE h, LPVOID b, DWORD n, LPDWORD rd, LPDWORD avail, LPDWORD left)
{
    (void)b; (void)n; Obj *o = H(h); if (!o) return FALSE;
    if (rd) *rd = 0;
    if (left) *left = 0;
    if (avail) *avail = 0;
    return TRUE;
}
WINAPI static BOOL k_DuplicateHandle(HANDLE sp, HANDLE sh, HANDLE tp, HANDLE *th, DWORD acc, BOOL inh, DWORD opt)
{
    (void)sp; (void)tp; (void)acc; (void)inh; (void)opt; Obj *o = H(sh);
    if (!o) { *th = sh; return TRUE; }
    if (o->type == O_FILE) { *th = file_handle(dup(o->fd)); return TRUE; }
    *th = sh; return TRUE;
}
WINAPI static BOOL k_SetHandleInformation(HANDLE h, DWORD m, DWORD f) { (void)h; (void)m; (void)f; return TRUE; }

/* ------------------------------------------------------------ synchronisation */
/* a critical section is {owner tid, depth}: all-zero memory is a valid unlocked section, as on Windows */
typedef struct { volatile DWORD owner; int depth; } CS;
WINAPI static void k_InitializeCriticalSection(void *cs) { memset(cs, 0, 40); }
WINAPI static BOOL k_InitializeCriticalSectionEx(void *cs, DWORD sp, DWORD f) { (void)sp; (void)f; memset(cs, 0, 40); return TRUE; }
WINAPI static BOOL k_InitializeCriticalSectionAndSpinCount(void *cs, DWORD sp) { (void)sp; memset(cs, 0, 40); return TRUE; }
WINAPI static BOOL k_TryEnterCriticalSection(void *p)
{
    CS *c = p; DWORD me = tid_self();
    if (c->owner == me) { c->depth++; return TRUE; }
    if (__sync_bool_compare_and_swap(&c->owner, 0, me)) { c->depth = 1; return TRUE; }
    return FALSE;
}
WINAPI static void k_EnterCriticalSection(void *p) { int spins = 0; while (!k_TryEnterCriticalSection(p)) { if (++spins > 50) { struct timespec ts = { 0, 100000 }; nanosleep(&ts, NULL); } else sched_yield(); } }
WINAPI static void k_LeaveCriticalSection(void *p) { CS *c = p; if (--c->depth <= 0) { c->depth = 0; __sync_synchronize(); c->owner = 0; } }
WINAPI static void k_DeleteCriticalSection(void *p) { (void)p; }

WINAPI static HANDLE k_CreateEventA(LPVOID sa, BOOL manual, BOOL init, LPCSTR name) { (void)sa; (void)name; Obj *o = calloc(1, sizeof *o); o->type = O_EVENT; o->manual = manual; o->signaled = init; return h_new(o); }
WINAPI static HANDLE k_CreateEventW(LPVOID sa, BOOL manual, BOOL init, LPCWSTR name) { return k_CreateEventA(sa, manual, init, (LPCSTR)name); }
WINAPI static BOOL k_SetEvent(HANDLE h) { Obj *o = H(h); if (!o || o->type != O_EVENT) return FALSE; lock(); o->signaled = 1; unlock(); return TRUE; }
WINAPI static BOOL k_ResetEvent(HANDLE h) { Obj *o = H(h); if (!o || o->type != O_EVENT) return FALSE; lock(); o->signaled = 0; unlock(); return TRUE; }
WINAPI static HANDLE k_CreateMutexA(LPVOID sa, BOOL own, LPCSTR name) { (void)sa; (void)name; Obj *o = calloc(1, sizeof *o); o->type = O_MUTEX; if (own) { o->owner = tid_self(); o->depth = 1; } return h_new(o); }
WINAPI static HANDLE k_CreateMutexW(LPVOID sa, BOOL own, LPCWSTR name) { return k_CreateMutexA(sa, own, (LPCSTR)name); }
WINAPI static BOOL k_ReleaseMutex(HANDLE h) { Obj *o = H(h); if (!o || o->type != O_MUTEX || o->owner != tid_self()) { set_last_error(288); return FALSE; } lock(); if (--o->depth == 0) o->owner = 0; unlock(); return TRUE; }
WINAPI static HANDLE k_CreateSemaphoreA(LPVOID sa, LONG init, LONG max, LPCSTR name) { (void)sa; (void)name; Obj *o = calloc(1, sizeof *o); o->type = O_SEM; o->count = init; o->max = max; return h_new(o); }
WINAPI static HANDLE k_CreateSemaphoreW(LPVOID sa, LONG init, LONG max, LPCWSTR name) { return k_CreateSemaphoreA(sa, init, max, (LPCSTR)name); }
WINAPI static BOOL k_ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) { Obj *o = H(h); if (!o || o->type != O_SEM) return FALSE; lock(); if (prev) *prev = o->count; o->count += n; unlock(); return TRUE; }

static int ready_locked(Obj *o, int take)
{
    switch (o->type) {
    case O_EVENT: if (!o->signaled) return 0; if (take && !o->manual) o->signaled = 0; return 1;
    case O_MUTEX: if (o->owner && o->owner != tid_self()) return 0; if (take) { o->owner = tid_self(); o->depth++; } return 1;
    case O_SEM: if (o->count <= 0) return 0; if (take) o->count--; return 1;
    case O_THREAD: return o->done;
    case O_FILE: return 1;
    default: return 1;
    }
}
static DWORD wait_proc(Obj *o, DWORD ms)
{
    if (o->done) return 0;
    int st;
    if (ms == 0xFFFFFFFF) { while (waitpid(o->pid, &st, 0) < 0 && errno == EINTR) {} }
    else {
        DWORD waited = 0;
        for (;;) {
            pid_t r = waitpid(o->pid, &st, WNOHANG);
            if (r == o->pid) break;
            if (r < 0 && errno != EINTR) break;
            if (waited >= ms) return 0x102;
            k_Sleep(2); waited += 2;
        }
    }
    o->done = 1; o->exitcode = WIFEXITED(st) ? (DWORD)WEXITSTATUS(st) : 0xC0000005u;
    return 0;
}
static DWORD wait_many(DWORD n, const HANDLE *hs, BOOL all, DWORD ms)
{
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    if (ms != 0xFFFFFFFF) { dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; } }
    for (DWORD i = 0; i < n; i++) { Obj *o = H(hs[i]); if (o && o->type == O_PROC) return wait_proc(o, ms); }
    lock();
    for (;;) {
        if (all) {
            int ok = 1; for (DWORD i = 0; i < n; i++) { Obj *o = H(hs[i]); if (o && !ready_locked(o, 0)) ok = 0; }
            if (ok) { for (DWORD i = 0; i < n; i++) { Obj *o = H(hs[i]); if (o) ready_locked(o, 1); } unlock(); return 0; }
        } else for (DWORD i = 0; i < n; i++) {
            Obj *o = H(hs[i]);
            if (!o) { pthread_mutex_unlock(&g_lock); set_last_error(6); return 0xFFFFFFFF; }
            if (ready_locked(o, 1)) { unlock(); return i; }
        }
        if (ms == 0) { pthread_mutex_unlock(&g_lock); return 0x102; }
        if (ms == 0xFFFFFFFF) pthread_cond_wait(&g_cv, &g_lock);
        else if (pthread_cond_timedwait(&g_cv, &g_lock, &dl) == ETIMEDOUT) { pthread_mutex_unlock(&g_lock); return 0x102; }
    }
}
WINAPI static DWORD k_WaitForSingleObject(HANDLE h, DWORD ms) { return wait_many(1, &h, FALSE, ms); }
WINAPI static DWORD k_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL a) { (void)a; return wait_many(1, &h, FALSE, ms); }
WINAPI static DWORD k_WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) { return wait_many(n, hs, all, ms); }

WINAPI static LONG k_InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
WINAPI static LONG k_InterlockedDecrement(volatile LONG *p) { return __sync_sub_and_fetch(p, 1); }
WINAPI static LONG k_InterlockedExchange(volatile LONG *p, LONG v) { return __sync_lock_test_and_set(p, v); }
WINAPI static LONG k_InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __sync_fetch_and_add(p, v); }
WINAPI static LONG k_InterlockedCompareExchange(volatile LONG *p, LONG x, LONG c) { return __sync_val_compare_and_swap(p, c, x); }

/* ------------------------------------------------------------ threads and TLS */
static void *thread_main(void *arg)
{
    Obj *o = arg;
    teb_init_thread(NULL);
    lock(); o->tid = tid_self(); while (o->suspended) pthread_cond_wait(&g_cv, &g_lock); unlock();
    DWORD r = ((DWORD (WINAPI *)(void *))o->fn)(o->arg);
    lock(); o->exitcode = r; o->done = 1; unlock();
    return NULL;
}
WINAPI static HANDLE k_CreateThread(LPVOID sa, size_t stack, void *fn, LPVOID arg, DWORD flags, LPDWORD tid)
{
    (void)sa; Obj *o = calloc(1, sizeof *o);
    o->type = O_THREAD; o->fn = fn; o->arg = arg; o->suspended = (flags & 4) != 0; o->exitcode = 0x103;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, stack >= 65536 ? stack : 1 << 20);
    HANDLE h = h_new(o);
    if (pthread_create(&o->th, &at, thread_main, o)) { k_CloseHandle(h); set_last_error(8); return NULL; }
    pthread_detach(o->th);
    if (tid) { lock(); while (!o->tid) { pthread_mutex_unlock(&g_lock); sched_yield(); pthread_mutex_lock(&g_lock); } *tid = o->tid; pthread_mutex_unlock(&g_lock); }
    return h;
}
WINAPI static DWORD k_ResumeThread(HANDLE h) { Obj *o = H(h); if (!o || o->type != O_THREAD) return 0xFFFFFFFF; lock(); int was = o->suspended; o->suspended = 0; unlock(); return (DWORD)was; }
WINAPI static BOOL k_GetExitCodeThread(HANDLE h, LPDWORD c) { Obj *o = H(h); if (!o || o->type != O_THREAD) return FALSE; *c = o->done ? o->exitcode : 0x103; return TRUE; }
WINAPI static void k_ExitThread(DWORD c) { (void)c; pthread_exit(NULL); }
WINAPI static BOOL k_SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return TRUE; }

static unsigned char tls_used[64]; static pthread_mutex_t tls_lk = PTHREAD_MUTEX_INITIALIZER;
static void **tls_slots(void) { return (void **)(uintptr_t)(*(void ***)teb_tls_slot()); }
WINAPI static DWORD k_TlsAlloc(void)
{
    pthread_mutex_lock(&tls_lk);
    for (DWORD i = 16; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; pthread_mutex_unlock(&tls_lk); return i; }   /* 0..15 belong to image TLS */
    pthread_mutex_unlock(&tls_lk); set_last_error(8); return 0xFFFFFFFF;
}
WINAPI static BOOL k_TlsFree(DWORD i) { if (i >= 64) return FALSE; tls_used[i] = 0; return TRUE; }
WINAPI static LPVOID k_TlsGetValue(DWORD i) { set_last_error(0); return i < 64 ? tls_slots()[i] : NULL; }
WINAPI static BOOL k_TlsSetValue(DWORD i, LPVOID v) { if (i >= 64) return FALSE; tls_slots()[i] = v; return TRUE; }

/* ------------------------------------------------------------ time */
WINAPI static void k_GetSystemTimeAsFileTime(uint64_t *ft) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); *ft = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100; }
WINAPI static DWORD k_GetTickCount(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
WINAPI static ULONGLONG k_GetTickCount64(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (ULONGLONG)ts.tv_sec * 1000 + (ULONGLONG)ts.tv_nsec / 1000000; }
WINAPI static BOOL k_QueryPerformanceCounter(LONGLONG *v) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); *v = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec; return TRUE; }
WINAPI static BOOL k_QueryPerformanceFrequency(LONGLONG *v) { *v = 1000000000LL; return TRUE; }
static void fill_systime(WORD *st, struct tm *t, long ms) { st[0] = (WORD)(t->tm_year + 1900); st[1] = (WORD)(t->tm_mon + 1); st[2] = (WORD)t->tm_wday; st[3] = (WORD)t->tm_mday; st[4] = (WORD)t->tm_hour; st[5] = (WORD)t->tm_min; st[6] = (WORD)t->tm_sec; st[7] = (WORD)ms; }
WINAPI static void k_GetSystemTime(WORD *st) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm t; gmtime_r(&ts.tv_sec, &t); fill_systime(st, &t, ts.tv_nsec / 1000000); }
WINAPI static void k_GetLocalTime(WORD *st) { k_GetSystemTime(st); }

/* ------------------------------------------------------------ modules, environment, command line */
static char cmdline_a[2048]; static WCHAR cmdline_w[2048];
static void build_cmdline(void)
{
    size_t l = 0;
    for (int i = 1; i < g_argc && l + 3 < sizeof cmdline_a; i++) {
        const char *a = i == 1 ? g_exe_path_win : g_argv[i];
        int q = i == 1 || strchr(a, ' ') || !*a;
        if (i > 1) cmdline_a[l++] = ' ';
        if (q) cmdline_a[l++] = '"';
        for (; *a && l + 3 < sizeof cmdline_a; a++) cmdline_a[l++] = *a;
        if (q) cmdline_a[l++] = '"';
    }
    cmdline_a[l] = 0; utf8_to_wide(cmdline_a, -1, cmdline_w, 2047);
}
WINAPI static LPSTR k_GetCommandLineA(void) { return cmdline_a; }
WINAPI static LPWSTR k_GetCommandLineW(void) { return cmdline_w; }
WINAPI static DWORD k_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) { (void)m; DWORD l = (DWORD)strlen(g_exe_path_win); if (l + 1 > n) { l = n - 1; } memcpy(out, g_exe_path_win, l); out[l] = 0; return l; }
WINAPI static DWORD k_GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD n) { (void)m; return (DWORD)utf8_to_wide(g_exe_path_win, -1, out, (int)n) - 1; }
static HMODULE builtin_mod(const char *n)
{
    static const char *names[] = { "kernel32", "kernelbase", "ntdll", "msvcrt", "user32", "advapi32", "shell32", "ws2_32", "ucrtbase", "gdi32", "ole32", "shlwapi", "psapi", NULL };
    char b[64]; size_t i = 0; for (; n[i] && i < 63; i++) b[i] = (char)tolower((unsigned char)n[i]); b[i] = 0;
    char *d = strstr(b, ".dll"); if (d) *d = 0;
    for (int k = 0; names[k]; k++) if (!strcmp(b, names[k])) return (HMODULE)(uintptr_t)(0x7FF000000000ull + (uint64_t)k * 0x10000);
    return NULL;
}
WINAPI static HMODULE k_GetModuleHandleA(LPCSTR n)
{
    if (!n) return g_main_module->base;
    HMODULE b = builtin_mod(n); if (b) return b;
    set_last_error(126); return NULL;
}
WINAPI static HMODULE k_GetModuleHandleW(LPCWSTR n) { if (!n) return g_main_module->base; char a[260]; wide_to_utf8(n, -1, a, sizeof a); return k_GetModuleHandleA(a); }
WINAPI static HMODULE k_LoadLibraryA(LPCSTR n)
{
    HMODULE b = builtin_mod(n); if (b) return b;
    char p[400]; if (!pe_find_dll(n, p, sizeof p)) { set_last_error(126); return NULL; }
    Module *m = pe_load(p, n, 1);
    if (!m) { set_last_error(126); return NULL; }
    pe_call_entry(m, 1);
    return m->base;
}
WINAPI static HMODULE k_LoadLibraryW(LPCWSTR n) { char a[260]; wide_to_utf8(n, -1, a, sizeof a); return k_LoadLibraryA(a); }
WINAPI static BOOL k_FreeLibrary(HMODULE m) { (void)m; return TRUE; }
WINAPI static FARPROC k_GetProcAddress(HMODULE m, LPCSTR name)
{
    if ((uintptr_t)name < 0x10000) { set_last_error(127); return NULL; }
    if ((uintptr_t)m >= 0x7FF000000000ull) { void *p = rt_resolve("", name); if (!p) set_last_error(127); return p; }
    extern Module *pe_module_by_base(void *); Module *mod = pe_module_by_base(m);
    void *p = mod ? pe_export(mod, name) : NULL;
    if (!p) set_last_error(127);
    return p;
}

static char *env_get_raw(const char *n) { return getenv(n); }
WINAPI static DWORD k_GetEnvironmentVariableA(LPCSTR n, LPSTR out, DWORD sz)
{
    const char *v = env_get_raw(n);
    if (!v) { set_last_error(203); return 0; }
    DWORD l = (DWORD)strlen(v);
    if (l + 1 > sz) return l + 1;
    memcpy(out, v, l + 1); return l;
}
WINAPI static DWORD k_GetEnvironmentVariableW(LPCWSTR n, LPWSTR out, DWORD sz)
{
    char a[260]; wide_to_utf8(n, -1, a, sizeof a); const char *v = env_get_raw(a);
    if (!v) { set_last_error(203); return 0; }
    int l = utf8_to_wide(v, -1, NULL, 0);
    if ((DWORD)l > sz) return (DWORD)l;
    utf8_to_wide(v, -1, out, (int)sz); return (DWORD)l - 1;
}
WINAPI static BOOL k_SetEnvironmentVariableA(LPCSTR n, LPCSTR v) { if (!v) return unsetenv(n) == 0; return setenv(n, v, 1) == 0; }
WINAPI static DWORD k_ExpandEnvironmentStringsA(LPCSTR src, LPSTR out, DWORD sz)
{
    char tmp[2048]; size_t l = 0;
    for (const char *p = src; *p && l + 1 < sizeof tmp; ) {
        if (*p == '%') {
            const char *e = strchr(p + 1, '%');
            if (e && e > p + 1) { char nm[128]; size_t k = (size_t)(e - p - 1); if (k < sizeof nm) { memcpy(nm, p + 1, k); nm[k] = 0; const char *v = getenv(nm); if (v) { for (; *v && l + 1 < sizeof tmp; v++) tmp[l++] = *v; p = e + 1; continue; } } }
        }
        tmp[l++] = *p++;
    }
    tmp[l] = 0;
    if (l + 1 <= sz) memcpy(out, tmp, l + 1);
    return (DWORD)l + 1;
}
extern char **environ;
WINAPI static LPSTR k_GetEnvironmentStringsA(void)
{
    size_t n = 1; for (char **e = environ; e && *e; e++) n += strlen(*e) + 1;
    char *b = malloc(n), *p = b; for (char **e = environ; e && *e; e++) { size_t l = strlen(*e) + 1; memcpy(p, *e, l); p += l; } *p = 0;
    return b;
}
WINAPI static BOOL k_FreeEnvironmentStringsA(LPSTR p) { free(p); return TRUE; }

WINAPI static void k_GetStartupInfoA(void *si) { memset(si, 0, 104); *(DWORD *)si = 104; }
WINAPI static LPVOID k_SetUnhandledExceptionFilter(LPVOID f) { (void)f; return NULL; }
WINAPI static LONG k_UnhandledExceptionFilter(LPVOID p) { (void)p; return 0; }
WINAPI static BOOL k_IsDBCSLeadByteEx(DWORD cp, BYTE b) { (void)cp; (void)b; return FALSE; }
WINAPI static BOOL k_IsDebuggerPresent(void) { return FALSE; }
WINAPI static int k_MultiByteToWideChar(DWORD cp, DWORD fl, LPCSTR s, int n, LPWSTR out, int cap) { (void)cp; (void)fl; return out && cap ? utf8_to_wide(s, n, out, cap) : utf8_to_wide(s, n, NULL, 0); }
WINAPI static int k_WideCharToMultiByte(DWORD cp, DWORD fl, LPCWSTR w, int n, LPSTR out, int cap, LPCSTR def, BOOL *used)
{
    (void)cp; (void)fl; (void)def; if (used) *used = 0;
    size_t need = (size_t)(n < 0 ? 1 : n) * 4 + 5; if (n < 0) { n = 0; while (w[n]) n++; n++; }
    char *tmp = malloc((size_t)n * 4 + 5); wide_to_utf8(w, n, tmp, (size_t)n * 4 + 5); (void)need;
    int l = (int)strlen(tmp);
    int total = l + 1; if (w[n - 1] != 0) total = l;                                 /* explicit length: no terminator */
    if (!out || !cap) { free(tmp); return total; }
    if (total > cap) { free(tmp); set_last_error(122); return 0; }
    memcpy(out, tmp, (size_t)total); free(tmp); return total;
}
WINAPI static DWORD k_GetVersion(void) { return 0x0A00 | (0x4A61 << 16); }
WINAPI static BOOL k_GetVersionExA(void *v) { DWORD *d = v; if (d[0] < 20) return FALSE; d[1] = 10; d[2] = 0; d[3] = 19045; d[4] = 2; return TRUE; }
WINAPI static BOOL k_GetComputerNameA(LPSTR b, LPDWORD n) { const char *h = "NEOS"; if (*n < 5) { *n = 5; return FALSE; } strcpy(b, h); *n = 4; return TRUE; }
WINAPI static DWORD k_GetLogicalDrives(void) { return 1 << 2; }
WINAPI static DWORD k_GetACP(void) { return 65001; }
WINAPI static DWORD k_lstrlenA(LPCSTR s) { return (DWORD)strlen(s); }
WINAPI static LPSTR k_lstrcpyA(LPSTR d, LPCSTR s) { return strcpy(d, s); }
WINAPI static BOOL k_GetProcessTimes(HANDLE h, void *a, void *b, void *c, void *d) { (void)h; memset(a, 0, 8); memset(b, 0, 8); memset(c, 0, 8); memset(d, 0, 8); return TRUE; }
WINAPI static BOOL k_GetExitCodeProcess(HANDLE h, LPDWORD c)
{
    Obj *o = H(h); if (!o || o->type != O_PROC) return FALSE;
    if (!o->done) { int st; pid_t r = waitpid(o->pid, &st, WNOHANG); if (r == o->pid) { o->done = 1; o->exitcode = WIFEXITED(st) ? (DWORD)WEXITSTATUS(st) : 0xC0000005u; } }
    *c = o->done ? o->exitcode : 259; return TRUE;
}

/* ------------------------------------------------------------ CreateProcess: fork + exec; the kernel sends PE files to winrt itself */
static char **split_cmdline(const char *cl, int *argc)
{
    char **v = calloc(64, sizeof *v); int n = 0; const char *p = cl;
    while (*p && n < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *a = malloc(strlen(p) + 1); size_t l = 0; int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') q = !q; else a[l++] = *p; p++; }
        a[l] = 0; v[n++] = a;
    }
    *argc = n; return v;
}
static BOOL create_process(LPCSTR app, LPSTR cmd, void *si, void *pi)
{
    int argc; char **argv = split_cmdline(cmd ? cmd : app, &argc);
    if (!argc) { set_last_error(87); return FALSE; }
    char path[300]; lp(app ? app : argv[0], path);
    struct stat st;
    if (stat(path, &st) && !strchr(app ? app : argv[0], '\\') && !strchr(app ? app : argv[0], '/')) {
        const char *tries[] = { "/bin/%s", "/bin/%s.exe", "/mnt/c/Windows/%s", "/mnt/c/Windows/%s.exe", "%s.exe", NULL };
        for (int i = 0; tries[i]; i++) { snprintf(path, sizeof path, tries[i], argv[0]); if (!stat(path, &st)) break; path[0] = 0; }
    } else if (stat(path, &st)) { size_t l = strlen(path); if (l + 5 < sizeof path) { strcat(path, ".exe"); if (stat(path, &st)) path[0] = 0; } }
    if (!path[0]) { set_last_error(2); return FALSE; }
    DWORD flags = si ? *(DWORD *)((char *)si + 60) : 0;
    HANDLE hin = si ? *(HANDLE *)((char *)si + 80) : 0, hout = si ? *(HANDLE *)((char *)si + 88) : 0, herr = si ? *(HANDLE *)((char *)si + 96) : 0;
    crt_flush_all();
    pid_t pid = fork();
    if (pid < 0) return fail_errno();
    if (!pid) {
        if (flags & 0x100) {
            Obj *o;
            if ((o = H(hin)) && o->type == O_FILE) dup2(o->fd, 0);
            if ((o = H(hout)) && o->type == O_FILE) dup2(o->fd, 1);
            if ((o = H(herr)) && o->type == O_FILE) dup2(o->fd, 2);
        }
        for (int fd = 3; fd < 256; fd++) close(fd);                                /* only the standard handles are inherited */
        argv[0] = path;
        execv(path, argv);
        _exit(127);
    }
    Obj *o = calloc(1, sizeof *o); o->type = O_PROC; o->pid = pid;
    HANDLE hp = h_new(o);
    Obj *t = calloc(1, sizeof *t); t->type = O_THREAD; t->done = 0; HANDLE ht = h_new(t);
    if (pi) { *(HANDLE *)pi = hp; *(HANDLE *)((char *)pi + 8) = ht; *(DWORD *)((char *)pi + 16) = (DWORD)pid; *(DWORD *)((char *)pi + 20) = (DWORD)pid; }
    return TRUE;
}
WINAPI static BOOL k_CreateProcessA(LPCSTR app, LPSTR cmd, LPVOID pa, LPVOID ta, BOOL inh, DWORD fl, LPVOID env, LPCSTR cwd, void *si, void *pi)
{ (void)pa; (void)ta; (void)inh; (void)fl; (void)env; (void)cwd; return create_process(app, cmd, si, pi); }
WINAPI static BOOL k_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPVOID pa, LPVOID ta, BOOL inh, DWORD fl, LPVOID env, LPCWSTR cwd, void *si, void *pi)
{
    (void)pa; (void)ta; (void)inh; (void)fl; (void)env; (void)cwd;
    char a[300], c[2048]; if (app) wide_to_utf8(app, -1, a, sizeof a); if (cmd) wide_to_utf8(cmd, -1, c, sizeof c);
    return create_process(app ? a : NULL, cmd ? c : NULL, si, pi);
}

/* ------------------------------------------------------------ ntdll / SEH stubs (programs are run without structured exception unwinding) */
WINAPI static int k_C_specific_handler(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; return 1; }
WINAPI static void k_RtlCaptureContext(void *c) { memset(c, 0, 1232); }
WINAPI static void *k_RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *h) { (void)pc; (void)h; if (base) *base = 0; return NULL; }
WINAPI static BOOL k_RtlAddFunctionTable(void *t, DWORD n, uint64_t b) { (void)t; (void)n; (void)b; return TRUE; }
WINAPI static void k_RaiseException(DWORD c, DWORD f, DWORD n, const void *a) { (void)f; (void)n; (void)a; fprintf(stderr, "winrt: unhandled Windows exception 0x%08X\n", c); crt_flush_all(); _exit(3); }
WINAPI static DWORD k_RtlNtStatusToDosError(DWORD s) { (void)s; return 31; }
WINAPI static DWORD k_NtQueryInformationProcess(void) { return 0xC0000002u; }
WINAPI static BOOL k_GetUserNameA(LPSTR b, LPDWORD n) { if (*n < 6) { *n = 6; return FALSE; } strcpy(b, "neos"); *n = 5; return TRUE; }
WINAPI static BOOL k_FlushInstructionCache(HANDLE p, LPCVOID b, size_t n) { (void)p; (void)b; (void)n; return TRUE; }
WINAPI static BOOL k_Beep(DWORD f, DWORD d) { (void)f; (void)d; return TRUE; }
WINAPI static void k_InitializeSListHead(void *p) { memset(p, 0, 16); }
WINAPI static BOOL k_DisableThreadLibraryCalls(HMODULE m) { (void)m; return TRUE; }

/* ------------------------------------------------------------ export table */
typedef struct { const char *name; void *fn; } Sym;
#define K(n) { #n, (void *)k_##n }
static const Sym ksyms[] = {
    K(GetLastError), K(SetLastError), K(ExitProcess), K(TerminateProcess), K(GetCurrentProcess), K(GetCurrentThread), K(GetCurrentProcessId), K(GetCurrentThreadId),
    K(Sleep), K(SwitchToThread), K(GetProcessHeap), K(HeapCreate), K(HeapDestroy), K(HeapAlloc), K(HeapReAlloc), K(HeapFree), K(HeapSize),
    K(LocalAlloc), K(LocalFree), K(GlobalAlloc), K(GlobalFree), K(VirtualAlloc), K(VirtualFree), K(VirtualProtect), K(VirtualQuery), K(GetSystemInfo),
    K(CreateFileA), K(CreateFileW), K(CloseHandle), K(ReadFile), K(WriteFile), K(SetFilePointer), K(SetFilePointerEx), K(GetFileSize), K(GetFileSizeEx),
    K(FlushFileBuffers), K(SetEndOfFile), K(GetFileType), K(DeleteFileA), K(DeleteFileW), K(CreateDirectoryA), K(CreateDirectoryW), K(RemoveDirectoryA),
    K(MoveFileA), K(MoveFileExA), K(CopyFileA), K(GetFileAttributesA), K(GetFileAttributesW), K(GetCurrentDirectoryA), K(SetCurrentDirectoryA),
    K(GetFullPathNameA), K(GetTempPathA), K(FindFirstFileA), K(FindNextFileA), K(FindClose),
    K(GetStdHandle), K(SetStdHandle), K(GetConsoleMode), K(SetConsoleMode), K(WriteConsoleA), K(WriteConsoleW), K(ReadConsoleA), K(SetConsoleTextAttribute),
    K(SetConsoleTitleA), K(SetConsoleCtrlHandler), K(GetConsoleScreenBufferInfo), K(GetConsoleCP), K(SetConsoleCP), K(OutputDebugStringA),
    K(CreatePipe), K(PeekNamedPipe), K(DuplicateHandle), K(SetHandleInformation),
    K(InitializeCriticalSection), K(InitializeCriticalSectionEx), K(InitializeCriticalSectionAndSpinCount), K(EnterCriticalSection), K(LeaveCriticalSection),
    K(TryEnterCriticalSection), K(DeleteCriticalSection), K(CreateEventA), K(CreateEventW), K(SetEvent), K(ResetEvent), K(CreateMutexA), K(CreateMutexW),
    K(ReleaseMutex), K(CreateSemaphoreA), K(CreateSemaphoreW), K(ReleaseSemaphore), K(WaitForSingleObject), K(WaitForSingleObjectEx), K(WaitForMultipleObjects),
    K(InterlockedIncrement), K(InterlockedDecrement), K(InterlockedExchange), K(InterlockedExchangeAdd), K(InterlockedCompareExchange),
    K(CreateThread), K(ResumeThread), K(GetExitCodeThread), K(ExitThread), K(SetThreadPriority), K(TlsAlloc), K(TlsFree), K(TlsGetValue), K(TlsSetValue),
    K(GetSystemTimeAsFileTime), K(GetTickCount), K(GetTickCount64), K(QueryPerformanceCounter), K(QueryPerformanceFrequency), K(GetSystemTime), K(GetLocalTime),
    K(GetCommandLineA), K(GetCommandLineW), K(GetModuleFileNameA), K(GetModuleFileNameW), K(GetModuleHandleA), K(GetModuleHandleW), K(LoadLibraryA), K(LoadLibraryW),
    K(FreeLibrary), K(GetProcAddress), K(GetEnvironmentVariableA), K(GetEnvironmentVariableW), K(SetEnvironmentVariableA), K(ExpandEnvironmentStringsA),
    K(GetEnvironmentStringsA), K(FreeEnvironmentStringsA), K(GetStartupInfoA), K(SetUnhandledExceptionFilter), K(UnhandledExceptionFilter), K(IsDBCSLeadByteEx),
    K(IsDebuggerPresent), K(MultiByteToWideChar), K(WideCharToMultiByte), K(GetVersion), K(GetVersionExA), K(GetComputerNameA), K(GetLogicalDrives), K(GetACP),
    K(lstrlenA), K(lstrcpyA), K(GetProcessTimes), K(GetExitCodeProcess), K(CreateProcessA), K(CreateProcessW), K(RaiseException), K(GetUserNameA),
    K(FlushInstructionCache), K(Beep), K(InitializeSListHead), K(DisableThreadLibraryCalls),
    { "__C_specific_handler", (void *)k_C_specific_handler }, { "RtlCaptureContext", (void *)k_RtlCaptureContext }, { "RtlLookupFunctionEntry", (void *)k_RtlLookupFunctionEntry },
    { "RtlAddFunctionTable", (void *)k_RtlAddFunctionTable }, { "RtlNtStatusToDosError", (void *)k_RtlNtStatusToDosError },
    { "NtQueryInformationProcess", (void *)k_NtQueryInformationProcess }, { "ExitThread", (void *)k_ExitThread },
    { NULL, NULL }
};
extern void *crt_resolve(const char *name);
void k32_init(void)
{
    std_init();
    build_cmdline();
    peb[0x10] = 0; *(uint64_t *)(peb + 0x10) = 0;
    *(DWORD *)(peb + 0x118) = 10; *(DWORD *)(peb + 0x11c) = 0; *(WORD *)(peb + 0x120) = 19045;
    (void)dummy_key; (void)g_argi;
}
void k32_list(void (*cb)(const char *))
{
    for (const Sym *s = ksyms; s->name; s++) cb(s->name);
    extern void crt_list(void (*)(const char *)); crt_list(cb);
    extern void ext_list(void (*)(const char *)); ext_list(cb);
}
void *rt_resolve(const char *dll, const char *name)
{
    (void)dll;
    for (const Sym *s = ksyms; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    void *p = crt_resolve(name);
    if (p) return p;
    extern void *ext_resolve(const char *); return ext_resolve(name);
}
