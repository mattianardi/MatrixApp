/* msvcrt shim: stdio (with a Windows-compatible printf engine), strings, math, startup hooks. */
#include "winrt.h"
#include <wchar.h>
#include <locale.h>
#include <sys/time.h>

typedef __builtin_ms_va_list mva;

void crt_flush_all(void) { fflush(NULL); }

/* FILE objects as the program sees them are 48-byte blobs; the host FILE* lives in the first word. */
static uint8_t iob[3][48];
static FILE *HF(void *p) { return p ? *(FILE **)p : NULL; }
static void *new_file(FILE *f) { if (!f) return NULL; void **b = calloc(1, 48); b[0] = f; return b; }

/* ------------------------------------------------------------ printf engine */
typedef struct { char *buf; size_t len, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->buf = realloc(b->buf, b->cap); }
    memcpy(b->buf + b->len, s, n); b->len += n; b->buf[b->len] = 0;
}
static void wide_to_u8(const WCHAR *w, Buf *o)
{
    for (; *w; w++) {
        uint32_t c = *w; char t[4]; size_t k;
        if (c < 0x80) { t[0] = (char)c; k = 1; }
        else if (c < 0x800) { t[0] = (char)(0xC0 | c >> 6); t[1] = (char)(0x80 | (c & 63)); k = 2; }
        else { t[0] = (char)(0xE0 | c >> 12); t[1] = (char)(0x80 | ((c >> 6) & 63)); t[2] = (char)(0x80 | (c & 63)); k = 3; }
        bput(o, t, k);
    }
}
static void vfmt(Buf *o, const char *f, mva ap)
{
    for (; *f; f++) {
        if (*f != '%') { bput(o, f, 1); continue; }
        if (f[1] == '%') { bput(o, "%", 1); f++; continue; }
        char spec[48]; size_t sl = 0; spec[sl++] = '%'; f++;
        while (*f && strchr("-+ #0", *f) && sl < 40) spec[sl++] = *f++;
        if (*f == '*') { sl += (size_t)snprintf(spec + sl, 8, "%d", __builtin_va_arg(ap, int)); f++; }
        else while (isdigit((unsigned char)*f) && sl < 40) spec[sl++] = *f++;
        if (*f == '.') { spec[sl++] = *f++; if (*f == '*') { int p = __builtin_va_arg(ap, int); sl += (size_t)snprintf(spec + sl, 8, "%d", p < 0 ? 0 : p); f++; } else while (isdigit((unsigned char)*f) && sl < 44) spec[sl++] = *f++; }
        int lm = 0;                                              /* 0 int, 1 long(32), 2 64-bit, -1 short, -2 char, 3 wide */
        for (;;) {
            if (*f == 'l' && f[1] == 'l') { lm = 2; f += 2; }
            else if (*f == 'l') { lm = lm == 3 ? 3 : 1; f++; if (!strchr("diouxXcs", *f)) lm = 1; else if (*f == 'c' || *f == 's') lm = 3; }
            else if (*f == 'h' && f[1] == 'h') { lm = -2; f += 2; }
            else if (*f == 'h') { lm = -1; f++; }
            else if (!strncmp(f, "I64", 3)) { lm = 2; f += 3; }
            else if (!strncmp(f, "I32", 3)) { lm = 0; f += 3; }
            else if (*f == 'z' || *f == 'j' || *f == 't' || *f == 'I') { lm = 2; f++; }
            else if (*f == 'L') { f++; }
            else if (*f == 'w') { lm = 3; f++; }
            else break;
        }
        char tmp[512]; int n;
        switch (*f) {
        case 'd': case 'i': {
            long long v = lm == 2 ? __builtin_va_arg(ap, long long) : (long long)__builtin_va_arg(ap, int);
            if (lm == -1) v = (short)v; else if (lm == -2) v = (signed char)v;
            strcpy(spec + sl, "lld"); n = snprintf(tmp, sizeof tmp, spec, v); bput(o, tmp, (size_t)n); break; }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lm == 2 ? __builtin_va_arg(ap, unsigned long long) : (unsigned long long)__builtin_va_arg(ap, unsigned);
            if (lm == -1) v = (unsigned short)v; else if (lm == -2) v = (unsigned char)v;
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = *f; spec[sl] = 0; n = snprintf(tmp, sizeof tmp, spec, v); bput(o, tmp, (size_t)n); break; }
        case 'c': case 'C': {
            int c = __builtin_va_arg(ap, int);
            if (lm == 3 || *f == 'C') { WCHAR w[2] = { (WCHAR)c, 0 }; wide_to_u8(w, o); } else { char ch = (char)c; bput(o, &ch, 1); }
            break; }
        case 's': case 'S': {
            void *p = __builtin_va_arg(ap, void *);
            int wide = (lm == 3) != (*f == 'S');
            Buf t = { 0 };
            if (!p) bput(&t, "(null)", 6); else if (wide) wide_to_u8(p, &t); else bput(&t, p, strlen(p));
            spec[sl++] = 's'; spec[sl] = 0;
            char *dyn = malloc(t.len + 512); n = snprintf(dyn, t.len + 512, spec, t.buf ? t.buf : ""); bput(o, dyn, (size_t)n); free(dyn); free(t.buf); break; }
        case 'p': { void *v = __builtin_va_arg(ap, void *); n = snprintf(tmp, sizeof tmp, "%016llX", (unsigned long long)(uintptr_t)v); bput(o, tmp, (size_t)n); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = __builtin_va_arg(ap, double); spec[sl++] = *f; spec[sl] = 0; n = snprintf(tmp, sizeof tmp, spec, d); bput(o, tmp, (size_t)n); break; }
        case 'n': (void)__builtin_va_arg(ap, void *); break;
        case 0: return;
        default: bput(o, spec, sl); bput(o, f, 1); break;
        }
    }
}
static int out_file(FILE *f, const char *fmt, mva ap) { Buf b = { 0 }; vfmt(&b, fmt, ap); int n = (int)b.len; if (n) fwrite(b.buf, 1, b.len, f); free(b.buf); return n; }
WINAPI static int c_vfprintf(void *f, const char *fmt, mva ap) { return out_file(HF(f), fmt, ap); }
WINAPI static int c_vprintf(const char *fmt, mva ap) { return out_file(stdout, fmt, ap); }
WINAPI static int c_fprintf(void *f, const char *fmt, ...) { mva ap; __builtin_ms_va_start(ap, fmt); int r = out_file(HF(f), fmt, ap); __builtin_ms_va_end(ap); return r; }
WINAPI static int c_printf(const char *fmt, ...) { mva ap; __builtin_ms_va_start(ap, fmt); int r = out_file(stdout, fmt, ap); __builtin_ms_va_end(ap); return r; }
static int out_str(char *dst, size_t cap, const char *fmt, mva ap)
{
    Buf b = { 0 }; vfmt(&b, fmt, ap); int n = (int)b.len;
    if (dst && cap) { size_t k = b.len < cap - 1 ? b.len : cap - 1; if (b.buf) memcpy(dst, b.buf, k); dst[k] = 0; }
    free(b.buf); return n;
}
WINAPI static int c_vsprintf(char *d, const char *fmt, mva ap) { return out_str(d, (size_t)-1 >> 1, fmt, ap); }
WINAPI static int c_vsnprintf(char *d, size_t n, const char *fmt, mva ap) { return out_str(d, n, fmt, ap); }
WINAPI static int c_sprintf(char *d, const char *fmt, ...) { mva ap; __builtin_ms_va_start(ap, fmt); int r = out_str(d, (size_t)-1 >> 1, fmt, ap); __builtin_ms_va_end(ap); return r; }
WINAPI static int c_snprintf(char *d, size_t n, const char *fmt, ...) { mva ap; __builtin_ms_va_start(ap, fmt); int r = out_str(d, n, fmt, ap); __builtin_ms_va_end(ap); return r; }
WINAPI static int c__snprintf(char *d, size_t n, const char *fmt, ...) { mva ap; __builtin_ms_va_start(ap, fmt); int r = out_str(d, n, fmt, ap); __builtin_ms_va_end(ap); return r; }

/* scanf family: pointer arguments are forwarded to the host; Windows' 32-bit "l" is rewritten so it matches */
static void scan_fmt(const char *f, char *out, size_t cap)
{
    size_t l = 0;
    for (; *f && l + 4 < cap; f++) {
        if (*f == '%' && f[1] != '*' && f[1] != '%') {
            out[l++] = *f++;
            while (isdigit((unsigned char)*f)) out[l++] = *f++;
            if (*f == 'l' && strchr("diouxX", f[1])) f++;
            else if (!strncmp(f, "I64", 3)) { out[l++] = 'l'; out[l++] = 'l'; f += 3; }
            out[l++] = *f; if (!*f) break;
        } else out[l++] = *f;
    }
    out[l] = 0;
}
WINAPI static int c_sscanf(const char *s, const char *fmt, ...)
{
    mva ap; __builtin_ms_va_start(ap, fmt); void *a[8]; for (int i = 0; i < 8; i++) a[i] = __builtin_va_arg(ap, void *);
    char f[256]; scan_fmt(fmt, f, sizeof f); return sscanf(s, f, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}
WINAPI static int c_fscanf(void *fp, const char *fmt, ...)
{
    mva ap; __builtin_ms_va_start(ap, fmt); void *a[8]; for (int i = 0; i < 8; i++) a[i] = __builtin_va_arg(ap, void *);
    char f[256]; scan_fmt(fmt, f, sizeof f); return fscanf(HF(fp), f, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}
WINAPI static int c_scanf(const char *fmt, ...)
{
    mva ap; __builtin_ms_va_start(ap, fmt); void *a[8]; for (int i = 0; i < 8; i++) a[i] = __builtin_va_arg(ap, void *);
    char f[256]; scan_fmt(fmt, f, sizeof f); return scanf(f, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

/* ------------------------------------------------------------ stdio */
WINAPI static void *c___iob_func(void)
{
    static int init;
    if (!init) { init = 1; *(FILE **)iob[0] = stdin; *(FILE **)iob[1] = stdout; *(FILE **)iob[2] = stderr; }
    return iob;
}
WINAPI static void *c_fopen(const char *n, const char *m) { char p[300]; path_win_to_lin(n, p, sizeof p); char mm[16]; size_t k = 0; for (; m[k] && k < 15; k++) mm[k] = m[k] == 'b' || m[k] == 't' ? (m[k] == 'b' ? 'b' : 'b') : m[k]; mm[k] = 0; FILE *f = fopen(p, mm); if (!f) set_last_error(errno_to_win(errno)); return new_file(f); }
WINAPI static int c_fclose(void *f) { int r = fclose(HF(f)); free(f); return r; }
WINAPI static int c_fflush(void *f) { return fflush(f ? HF(f) : NULL); }
WINAPI static size_t c_fwrite(const void *b, size_t s, size_t n, void *f) { return fwrite(b, s, n, HF(f)); }
WINAPI static size_t c_fread(void *b, size_t s, size_t n, void *f) { if (HF(f) == stdin) fflush(stdout); return fread(b, s, n, HF(f)); }
WINAPI static int c_fputc(int c, void *f) { return fputc(c, HF(f)); }
WINAPI static int c_putc(int c, void *f) { return fputc(c, HF(f)); }
WINAPI static int c_fputs(const char *s, void *f) { return fputs(s, HF(f)); }
WINAPI static int c_puts(const char *s) { return puts(s); }
WINAPI static int c_putchar(int c) { return putchar(c); }
WINAPI static int c_fgetc(void *f) { if (HF(f) == stdin) fflush(stdout); return fgetc(HF(f)); }
WINAPI static int c_getc(void *f) { return c_fgetc(f); }
WINAPI static int c_getchar(void) { fflush(stdout); return getchar(); }
WINAPI static char *c_fgets(char *b, int n, void *f) { if (HF(f) == stdin) fflush(stdout); return fgets(b, n, HF(f)); }
WINAPI static int c_ungetc(int c, void *f) { return ungetc(c, HF(f)); }
WINAPI static int c_fseek(void *f, long o, int w) { return fseek(HF(f), o, w); }
WINAPI static int c__fseeki64(void *f, long long o, int w) { return fseeko(HF(f), (off_t)o, w); }
WINAPI static long c_ftell(void *f) { return ftell(HF(f)); }
WINAPI static long long c__ftelli64(void *f) { return ftello(HF(f)); }
WINAPI static void c_rewind(void *f) { rewind(HF(f)); }
WINAPI static int c_feof(void *f) { return feof(HF(f)); }
WINAPI static int c_ferror(void *f) { return ferror(HF(f)); }
WINAPI static void c_clearerr(void *f) { clearerr(HF(f)); }
WINAPI static int c_setvbuf(void *f, char *b, int m, size_t s) { return setvbuf(HF(f), b, m == 0x40 ? _IOFBF : m == 4 ? _IONBF : _IOLBF, s); }
WINAPI static int c__fileno(void *f) { return fileno(HF(f)); }
WINAPI static int c_remove(const char *n) { char p[300]; return remove(path_win_to_lin(n, p, sizeof p)); }
WINAPI static int c_rename(const char *a, const char *b) { char p[300], q[300]; return rename(path_win_to_lin(a, p, sizeof p), path_win_to_lin(b, q, sizeof q)); }
WINAPI static int c__setmode(int fd, int m) { (void)fd; (void)m; return 0x4000; }
WINAPI static int c__isatty(int fd) { return isatty(fd); }
WINAPI static int c__open(const char *n, int fl, ...)
{
    char p[300]; path_win_to_lin(n, p, sizeof p);
    int h = fl & 3; int fd = (fl & 0x100 ? O_CREAT : 0) | (fl & 0x200 ? O_TRUNC : 0) | (fl & 0x400 ? O_EXCL : 0) | (fl & 8 ? O_APPEND : 0) | h;
    return open(p, fd, 0644);
}
WINAPI static int c__close(int fd) { return close(fd); }
WINAPI static int c__read(int fd, void *b, unsigned n) { return (int)read(fd, b, n); }
WINAPI static int c__write(int fd, const void *b, unsigned n) { if (fd == 1 || fd == 2) fflush(NULL); return (int)write(fd, b, n); }
WINAPI static long c__lseek(int fd, long o, int w) { return (long)lseek(fd, o, w); }
WINAPI static char *c__getcwd(char *b, int n) { char l[300]; if (!getcwd(l, sizeof l)) return NULL; path_lin_to_win(l, b, (size_t)n); return b; }
WINAPI static int c__chdir(const char *p) { char l[300]; return chdir(path_win_to_lin(p, l, sizeof l)); }
WINAPI static int c__mkdir(const char *p) { char l[300]; return mkdir(path_win_to_lin(p, l, sizeof l), 0755); }
WINAPI static int c__rmdir(const char *p) { char l[300]; return rmdir(path_win_to_lin(p, l, sizeof l)); }
WINAPI static int c__access(const char *p, int m) { char l[300]; return access(path_win_to_lin(p, l, sizeof l), m & 6 ? (m & 2 ? W_OK : R_OK) : F_OK); }
WINAPI static int c__unlink(const char *p) { char l[300]; return unlink(path_win_to_lin(p, l, sizeof l)); }
WINAPI static int c_system(const char *cmd) { fflush(NULL); return cmd ? system(cmd) : 1; }

/* ------------------------------------------------------------ memory and strings */
#define F1(R, n, T1) WINAPI static R c_##n(T1 a) { return n(a); }
#define F2(R, n, T1, T2) WINAPI static R c_##n(T1 a, T2 b) { return n(a, b); }
#define F3(R, n, T1, T2, T3) WINAPI static R c_##n(T1 a, T2 b, T3 c) { return n(a, b, c); }
F1(void *, malloc, size_t) F2(void *, calloc, size_t, size_t) F2(void *, realloc, void *, size_t) WINAPI static void c_free(void *p) { free(p); }
F3(void *, memcpy, void *, const void *, size_t) F3(void *, memmove, void *, const void *, size_t) F3(void *, memset, void *, int, size_t) F3(int, memcmp, const void *, const void *, size_t)
F3(void *, memchr, const void *, int, size_t)
F1(size_t, strlen, const char *) F2(int, strcmp, const char *, const char *) F3(int, strncmp, const char *, const char *, size_t) F2(char *, strcpy, char *, const char *)
F3(char *, strncpy, char *, const char *, size_t) F2(char *, strcat, char *, const char *) F3(char *, strncat, char *, const char *, size_t)
F2(char *, strchr, const char *, int) F2(char *, strrchr, const char *, int) F2(char *, strstr, const char *, const char *) F1(char *, strdup, const char *)
F2(int, strcasecmp, const char *, const char *) F3(int, strncasecmp, const char *, const char *, size_t)
F2(size_t, strspn, const char *, const char *) F2(size_t, strcspn, const char *, const char *) F2(char *, strpbrk, const char *, const char *)
F1(char *, strerror, int) F1(int, atoi, const char *) F1(long, atol, const char *) F1(double, atof, const char *) F1(int, abs, int) F1(long, labs, long)
F1(int, toupper, int) F1(int, tolower, int) F1(int, isalpha, int) F1(int, isdigit, int) F1(int, isalnum, int) F1(int, isspace, int) F1(int, isupper, int)
F1(int, islower, int) F1(int, isprint, int) F1(int, ispunct, int) F1(int, isxdigit, int) F1(int, iscntrl, int)
WINAPI static size_t c_wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; } F1(char *, getenv, const char *)
F3(long, strtol, const char *, char **, int) F3(unsigned long, strtoul, const char *, char **, int) F2(double, strtod, const char *, char **)
F3(long long, strtoll, const char *, char **, int) F3(unsigned long long, strtoull, const char *, char **, int)
F2(char *, strtok, char *, const char *)
WINAPI static int c_rand(void) { return rand() & 0x7FFF; }
WINAPI static void c_srand(unsigned s) { srand(s); }
WINAPI static long long c__time64(long long *t) { time_t v = time(NULL); if (t) *t = v; return v; }
WINAPI static long long c_clock(void) { return clock() / (CLOCKS_PER_SEC / 1000); }
WINAPI static void *c__gmtime64(const long long *t) { time_t v = (time_t)*t; return gmtime(&v); }
WINAPI static void *c__localtime64(const long long *t) { time_t v = (time_t)*t; return localtime(&v); }
WINAPI static size_t c_strftime(char *b, size_t n, const char *f, const struct tm *t) { return strftime(b, n, f, t); }
WINAPI static int c_wcscmp(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
WINAPI static void c_qsort(void *base, size_t n, size_t sz, int (WINAPI *cmp)(const void *, const void *))
{
    char *a = base, *t = malloc(sz);                                      /* insertion sort when small, otherwise a heap sort with an ms_abi comparator */
    if (n < 24) { for (size_t i = 1; i < n; i++) { memcpy(t, a + i * sz, sz); size_t j = i; while (j > 0 && cmp(a + (j - 1) * sz, t) > 0) { memcpy(a + j * sz, a + (j - 1) * sz, sz); j--; } memcpy(a + j * sz, t, sz); } free(t); return; }
    #define AT(i) (a + (i) * sz)
    for (size_t s = n / 2; s-- > 0; ) { size_t r = s; for (;;) { size_t c = 2 * r + 1; if (c >= n) break; if (c + 1 < n && cmp(AT(c), AT(c + 1)) < 0) c++; if (cmp(AT(r), AT(c)) >= 0) break; memcpy(t, AT(r), sz); memcpy(AT(r), AT(c), sz); memcpy(AT(c), t, sz); r = c; } }
    for (size_t e = n - 1; e > 0; e--) { memcpy(t, AT(0), sz); memcpy(AT(0), AT(e), sz); memcpy(AT(e), t, sz); size_t r = 0; for (;;) { size_t c = 2 * r + 1; if (c >= e) break; if (c + 1 < e && cmp(AT(c), AT(c + 1)) < 0) c++; if (cmp(AT(r), AT(c)) >= 0) break; memcpy(t, AT(r), sz); memcpy(AT(r), AT(c), sz); memcpy(AT(c), t, sz); r = c; } }
    #undef AT
    free(t);
}
WINAPI static void *c_bsearch(const void *key, const void *base, size_t n, size_t sz, int (WINAPI *cmp)(const void *, const void *))
{
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; int c = cmp(key, (const char *)base + m * sz); if (!c) return (void *)((const char *)base + m * sz); if (c < 0) hi = m; else lo = m + 1; }
    return NULL;
}
WINAPI static int c__stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
WINAPI static int c__strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }

#define M1(n) WINAPI static double c_##n(double x) { return n(x); }
#define M2(n) WINAPI static double c_##n(double x, double y) { return n(x, y); }
M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(sqrt) M1(exp) M1(log) M1(log10) M1(floor) M1(ceil) M1(fabs) M1(sinh) M1(cosh) M1(tanh) M1(log2) M1(round) M1(trunc)
M2(pow) M2(atan2) M2(fmod) M2(fmax) M2(fmin) M2(hypot)

/* ------------------------------------------------------------ startup / exit */
static void (WINAPI *atexit_fns[64])(void); static int natexit;
WINAPI static void *c__onexit(void *f) { if (natexit < 64) atexit_fns[natexit++] = f; return f; }
WINAPI static int c_atexit(void *f) { return c__onexit(f) ? 0 : -1; }
static void run_atexit(void) { while (natexit) atexit_fns[--natexit](); }
WINAPI static void c_exit(int c) { run_atexit(); fflush(NULL); exit(c); }
WINAPI static void c__exit(int c) { fflush(NULL); _exit(c); }
WINAPI static void c__cexit(void) { run_atexit(); fflush(NULL); }
WINAPI static void c_abort(void) { fflush(NULL); fputs("\nabnormal program termination\n", stderr); _exit(3); }
WINAPI static void c__amsg_exit(int c) { fprintf(stderr, "runtime error R%d\n", c); _exit(255); }
WINAPI static void c__initterm(void (WINAPI **s)(void), void (WINAPI **e)(void)) { for (; s < e; s++) if (*s) (*s)(); }
WINAPI static int c__initterm_e(int (WINAPI **s)(void), int (WINAPI **e)(void)) { for (; s < e; s++) if (*s) { int r = (*s)(); if (r) return r; } return 0; }
WINAPI static void c___set_app_type(int t) { (void)t; }
WINAPI static void c___setusermatherr(void *f) { (void)f; }
WINAPI static void c__fpreset(void) {}
WINAPI static void *c_signal(int s, void *h) { (void)s; (void)h; return NULL; }
WINAPI static int c__lock_dummy(int n) { (void)n; return 0; }
WINAPI static void c__lock(int n) { (void)n; }
WINAPI static void c__unlock(int n) { (void)n; }
WINAPI static int *c__errno(void) { return &errno; }
WINAPI static int c___lc_codepage(void) { return 65001; }
WINAPI static int c___mb_cur_max(void) { return 1; }
WINAPI static void *c_localeconv(void) { return localeconv(); }
WINAPI static char *c_setlocale(int c, const char *l) { (void)c; (void)l; return "C"; }
WINAPI static int c___getmainargs(int *argc, char ***argv, char ***env, int wild, void *si)
{
    (void)wild; (void)si;
    static char **av; extern char **environ;
    av = calloc((size_t)g_argc + 1, sizeof *av);
    av[0] = strdup(g_exe_path_win);
    for (int i = 2; i < g_argc; i++) av[i - 1] = g_argv[i];
    *argc = g_argc - 1; *argv = av; *env = environ;
    return 0;
}
static int fmode_v, commode_v; static char **initenv_v;
WINAPI static int *c___p__fmode(void) { return &fmode_v; }
WINAPI static int *c___p__commode(void) { return &commode_v; }
WINAPI static int c_fputc_unused(int c) { return c; }
WINAPI static char ***c___p___initenv(void) { extern char **environ; initenv_v = environ; return &initenv_v; }
WINAPI static char ***c___p__environ(void) { extern char **environ; return &environ; }
WINAPI static char ***c___p___argv(void) { static char **v; return &v; }
WINAPI static int *c___p___argc(void) { return &g_argc; }
WINAPI static int c__get_osfhandle_dummy(int fd) { return fd; }

typedef struct { const char *name; void *fn; } Sym;
#define C(n) { #n, (void *)c_##n }
static const Sym csyms[] = {
    C(vfprintf), C(vprintf), C(fprintf), C(printf), C(vsprintf), C(vsnprintf), C(sprintf), C(snprintf), C(sscanf), C(fscanf), C(scanf),
    { "_snprintf", (void *)c__snprintf }, { "_vsnprintf", (void *)c_vsnprintf }, { "__iob_func", (void *)c___iob_func },
    C(fopen), C(fclose), C(fflush), C(fwrite), C(fread), C(fputc), C(putc), C(fputs), C(puts), C(putchar), C(fgetc), C(getc), C(getchar), C(fgets), C(ungetc),
    C(fseek), C(_fseeki64), C(ftell), C(_ftelli64), C(rewind), C(feof), C(ferror), C(clearerr), C(setvbuf), C(_fileno), C(remove), C(rename), C(_setmode),
    C(_isatty), C(_open), C(_close), C(_read), C(_write), C(_lseek), C(_getcwd), C(_chdir), C(_mkdir), C(_rmdir), C(_access), C(_unlink), C(system),
    C(malloc), C(calloc), C(realloc), C(free), C(memcpy), C(memmove), C(memset), C(memcmp), C(memchr), C(strlen), C(strcmp), C(strncmp), C(strcpy), C(strncpy),
    C(strcat), C(strncat), C(strchr), C(strrchr), C(strstr), C(strdup), C(strspn), C(strcspn), C(strpbrk), C(strerror), C(atoi), C(atol), C(atof), C(abs), C(labs),
    C(toupper), C(tolower), C(isalpha), C(isdigit), C(isalnum), C(isspace), C(isupper), C(islower), C(isprint), C(ispunct), C(isxdigit), C(iscntrl), C(wcslen),
    C(getenv), C(strtol), C(strtoul), C(strtod), C(strtoll), C(strtoull), C(strtok), C(rand), C(srand), C(_time64), C(clock), C(_gmtime64), C(_localtime64),
    C(strftime), C(wcscmp), C(qsort), C(bsearch), C(_stricmp), C(_strnicmp),
    { "_strdup", (void *)c_strdup }, { "stricmp", (void *)c__stricmp }, { "strnicmp", (void *)c__strnicmp }, { "time", (void *)c__time64 },
    { "_mkgmtime", (void *)c__time64 }, { "gmtime", (void *)c__gmtime64 }, { "localtime", (void *)c__localtime64 },
    C(sin), C(cos), C(tan), C(asin), C(acos), C(atan), C(sqrt), C(exp), C(log), C(log10), C(floor), C(ceil), C(fabs), C(sinh), C(cosh), C(tanh), C(log2), C(round),
    C(trunc), C(pow), C(atan2), C(fmod), C(fmax), C(fmin), C(hypot),
    C(atexit), C(_onexit), C(exit), C(_exit), C(_cexit), C(abort), C(_amsg_exit), C(_initterm), C(_initterm_e), C(__set_app_type), C(__setusermatherr),
    C(_fpreset), C(signal), C(_lock), C(_unlock), C(_errno), C(localeconv), C(setlocale), C(__getmainargs),
    { "___lc_codepage_func", (void *)c___lc_codepage }, { "___mb_cur_max_func", (void *)c___mb_cur_max },
    { "__p__fmode", (void *)c___p__fmode }, { "__p__commode", (void *)c___p__commode }, { "__p___initenv", (void *)c___p___initenv },
    { "__p__environ", (void *)c___p__environ }, { "__p___argc", (void *)c___p___argc },
    { "_fmode", &fmode_v }, { "_commode", &commode_v }, { "__initenv", &initenv_v },
    { NULL, NULL }
};
void *crt_resolve(const char *name)
{
    for (const Sym *s = csyms; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}
