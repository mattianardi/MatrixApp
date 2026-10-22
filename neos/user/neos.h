#pragma once
/* Minimal runtime for NEOS user programs (no libc). Entry: _start -> main(argc, argv, envp) -> exit. */
#include "../kernel/neos_api.h"

typedef unsigned long u64;
typedef long i64;

static inline i64 neos_sys(u64 n, u64 a, u64 b, u64 c, u64 d)
{
    i64 r;
    register u64 r10 __asm__("r10") = d;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "memory", "rcx", "r11");
    return r;
}

static inline unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static inline void neos_exit(i64 code) { neos_sys(USYS_EXIT, code, 0, 0, 0); for (;;) { } }
static inline i64 neos_write(const char *s) { return neos_sys(USYS_WRITE, 1, (u64)s, slen(s), 0); }
static inline i64 neos_getpid(void) { return neos_sys(USYS_GETPID, 0, 0, 0, 0); }
static inline void neos_sleep_ms(u64 ms) { neos_sys(USYS_SLEEP_MS, ms, 0, 0, 0); }
static inline u64 neos_time_ns(void) { return (u64)neos_sys(USYS_TIME_NS, 0, 0, 0, 0); }
static inline void *neos_mmap(u64 len) { i64 r = neos_sys(USYS_MMAP, len, 0, 0, 0); return r < 0 ? (void *)0 : (void *)r; }
static inline i64 neos_tool(const char *cmd, char *out, u64 outlen) { return neos_sys(USYS_TOOL, (u64)cmd, (u64)out, outlen, 0); }

static inline void put_dec(i64 v)
{
    char b[24]; int i = 23; b[i] = 0;
    int neg = v < 0; u64 u = neg ? (u64)-v : (u64)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    neos_write(b + i);
}
static inline void put_hex(u64 v)
{
    char b[20]; int i = 19; b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    neos_write("0x"); neos_write(b + i);
}

int main(int argc, char **argv, char **envp);
__attribute__((used)) static void c_start(u64 *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    neos_exit(main(argc, argv, argv + argc + 1));
}
__asm__(".global _start\n_start:\n xor %ebp, %ebp\n mov %rsp, %rdi\n and $-16, %rsp\n call c_start\n");

static inline const char *getenv_(char **envp, const char *name)
{
    unsigned long n = slen(name);
    for (; *envp; envp++) {
        unsigned long i = 0;
        while (i < n && (*envp)[i] == name[i]) i++;
        if (i == n && (*envp)[n] == '=') return *envp + n + 1;
    }
    return (const char *)0;
}
