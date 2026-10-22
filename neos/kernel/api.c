#include "kernel.h"
#include "neos_api.h"
#include "varbus.h"

static long a_tune_get(const char *name, char *buf, unsigned long len)
{
    struct tunable *t = tune_find(name);
    if (!t) return -1;
    tune_format(t, buf, len);
    return (long)strlen(buf);
}
static long a_tune_set(const char *name, const char *value) { return tune_set(name, value, NULL); }
static long a_var_get(const char *name, char *buf, unsigned long len, int view) { return bus_get(name, view, buf, len); }
static long a_var_set(const char *name, const char *value, int view) { return bus_post_set(name, value, view); }
static void a_log(const char *m) { kprintf("%s\n", m); }
static unsigned long a_time(void) { return time_ns(); }

static const struct neos_api api = { NEOS_API_VERSION, a_log, a_time, a_tune_get, a_tune_set, a_var_get, a_var_set };
const struct neos_api *neos_api_get(void) { return &api; }

long neos_syscall(unsigned long n, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
    (void)d;
    switch (n) {
    case SYS_LOG:        a_log((const char *)a); return 0;
    case SYS_TIME_NS:    return (long)time_ns();
    case SYS_TUNE_GET:   return a_tune_get((const char *)a, (char *)b, c);
    case SYS_TUNE_SET:   return a_tune_set((const char *)a, (const char *)b);
    case SYS_VAR_GET:    return a_var_get((const char *)a, (char *)b, c, (int)d);
    case SYS_VAR_SET:    return a_var_set((const char *)a, (const char *)b, (int)c);
    case SYS_MODULE_CNT: return module_count();
    }
    return -38;     /* ENOSYS */
}
