#include "kernel.h"

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}
int strcasecmp(const char *a, const char *b)
{
    while (*a && to_lower(*a) == to_lower(*b)) { a++; b++; }
    return to_lower((u8)*a) - to_lower((u8)*b);
}
char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : NULL;
}
size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strlen(d);
    if (dl >= n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
void *memset(void *d, int c, size_t n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    u8 *p = d; const u8 *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

int parse_i64(const char *s, i64 *out)
{
    int neg = 0; u64 v = 0; unsigned base = 10;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    if (!*s) return -1;
    for (; *s; s++) {
        int d;
        if (is_digit(*s)) d = *s - '0';
        else if (base == 16 && to_lower(*s) >= 'a' && to_lower(*s) <= 'f') d = to_lower(*s) - 'a' + 10;
        else return -1;
        v = v * base + d;
    }
    *out = neg ? -(i64)v : (i64)v;
    return 0;
}

static void pad(fmt_out out, void *ctx, int n, char c) { while (n-- > 0) out(c, ctx); }

void kvformat(fmt_out out, void *ctx, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { out(*f, ctx); continue; }
        f++;
        int left = 0, width = 0, prec = -1, lng = 0; char pc = ' ';
        for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') pc = '0'; else break; }
        if (*f == '*') { width = va_arg(ap, int); f++; }
        else while (is_digit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (is_digit(*f)) prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l' || *f == 'z') { lng++; f++; }
        switch (*f) {
        case 'c': pad(out, ctx, left ? 0 : width - 1, ' '); out((char)va_arg(ap, int), ctx);
                  pad(out, ctx, left ? width - 1 : 0, ' '); break;
        case 's': {
            const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
            int l = 0; while (s[l] && (prec < 0 || l < prec)) l++;
            if (!left) pad(out, ctx, width - l, ' ');
            for (int i = 0; i < l; i++) out(s[i], ctx);
            if (left) pad(out, ctx, width - l, ' ');
            break; }
        case 'd': case 'u': case 'x': case 'X': case 'p': {
            u64 v; int neg = 0; unsigned base = 10;
            if (*f == 'd') {
                long long sv = lng ? va_arg(ap, long long) : va_arg(ap, int);
                if (sv < 0) { neg = 1; v = -(u64)sv; } else v = sv;
            } else if (*f == 'p') { v = va_arg(ap, u64); base = 16; width = 18; pc = '0'; out('0', ctx); out('x', ctx); width -= 2; }
            else { v = lng ? va_arg(ap, u64) : va_arg(ap, unsigned); if (*f != 'u') base = 16; }
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char t[24]; int n = 0;
            do { t[n++] = dig[v % base]; v /= base; } while (v);
            int total = n + neg;
            if (!left && pc == ' ') pad(out, ctx, width - total, ' ');
            if (neg) out('-', ctx);
            if (!left && pc == '0') pad(out, ctx, width - total, '0');
            while (n) out(t[--n], ctx);
            if (left) pad(out, ctx, width - total, ' ');
            break; }
        case '%': out('%', ctx); break;
        default:  out('%', ctx); if (*f) out(*f, ctx); else return; break;
        }
    }
}

struct sn { char *b; size_t n, len; };
static void sn_out(char c, void *ctx)
{
    struct sn *s = ctx;
    if (s->len + 1 < s->n) s->b[s->len] = c;
    s->len++;
}
int ksnprintf(char *buf, size_t n, const char *fmt, ...)
{
    struct sn s = { buf, n, 0 };
    va_list ap; va_start(ap, fmt); kvformat(sn_out, &s, fmt, ap); va_end(ap);
    if (n) buf[s.len < n ? s.len : n - 1] = 0;
    return (int)s.len;
}

int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct sn s = { buf, n, 0 };
    kvformat(sn_out, &s, fmt, ap);
    if (n) buf[s.len < n ? s.len : n - 1] = 0;
    return (int)s.len;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int b64_encode(const void *in, size_t n, char *out, size_t on)
{
    const u8 *p = in; size_t o = 0;
    if (((n + 2) / 3) * 4 + 1 > on) return -1;
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = B64[v >> 18]; out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '='; out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}

int b64_decode(const char *in, void *out, size_t on)
{
    u8 *o = out; size_t n = 0; u32 acc = 0; int bits = 0;
    for (; *in && *in != '='; in++) {
        const char *p = strchr(B64, *in);
        if (!p || !*in) return -1;
        acc = acc << 6 | (u32)(p - B64); bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= on) return -1; o[n++] = (u8)(acc >> bits); }
    }
    return (int)n;
}
