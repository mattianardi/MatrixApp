#pragma once
/*
 * NEOS program API. Every kernel module that wants to be tunable or observable
 * by programs does it through this surface:
 *   - syscall  int 0x80  (rax = number, rdi/rsi/rdx/r10 = args, result in rax)
 *   - struct neos_api    (same functions as a vtable, for in-kernel modules/programs)
 * Pointers are kernel pointers for now; they must be validated and copied once
 * user mode (ring 3) exists.
 */
#define NEOS_SYSCALL_VECTOR 0x80
#define NEOS_YIELD_VECTOR   0x81
#define NEOS_API_VERSION    1

enum neos_view { NEOS_VIEW_NATIVE = 0, NEOS_VIEW_LINUX = 1, NEOS_VIEW_WINDOWS = 2 };

enum neos_sys {
    SYS_LOG        = 1,   /* (const char *msg)                           -> 0            */
    SYS_TIME_NS    = 2,   /* ()                                          -> monotonic ns */
    SYS_TUNE_GET   = 3,   /* (name, buf, len)                            -> strlen / <0  */
    SYS_TUNE_SET   = 4,   /* (name, value)                               -> 0 live, 1 next boot, <0 error */
    SYS_VAR_GET    = 5,   /* (name, buf, len, enum neos_view)            -> strlen / <0  */
    SYS_VAR_SET    = 6,   /* (name, value, enum neos_view)               -> 0 (applied asynchronously by the bus thread) */
    SYS_MODULE_CNT = 7,   /* ()                                          -> number of registered modules */
};

long neos_syscall(unsigned long n, unsigned long a, unsigned long b, unsigned long c, unsigned long d);

struct neos_api {
    unsigned version;
    void (*log)(const char *msg);
    unsigned long (*time_ns)(void);
    long (*tune_get)(const char *name, char *buf, unsigned long len);
    long (*tune_set)(const char *name, const char *value);
    long (*var_get)(const char *name, char *buf, unsigned long len, int view);
    long (*var_set)(const char *name, const char *value, int view);
};
const struct neos_api *neos_api_get(void);

/* ---- ABI for user programs (ring 3): `int 0x80`, rax = number, rdi rsi rdx r10 = args, result in rax ---- */
enum neos_usys {
    USYS_EXIT     = 1,   /* (code)                          does not return                      */
    USYS_WRITE    = 2,   /* (fd 1|2, buf, len)              -> bytes written / -EFAULT           */
    USYS_GETPID   = 3,
    USYS_YIELD    = 4,
    USYS_SLEEP_MS = 5,   /* (ms)                                                                 */
    USYS_TIME_NS  = 6,   /* ()                              -> monotonic ns                      */
    USYS_MMAP     = 7,   /* (len)                           -> address of fresh zeroed RW pages  */
    USYS_TOOL     = 8,   /* (cmdline, out, outlen)          -> 0 or error code; the same tools an agent has, under this process's capabilities */
};
#define NEOS_EFAULT 14
#define NEOS_ENOMEM 12
#define NEOS_ENOSYS 38
