#include "neos.h"

static const char *const greetings[] = { "hello", "from", "ring 3" };      /* pointer table: needs relocation when built as PIE */

int main(int argc, char **argv, char **envp)
{
    volatile int k = 0;                                         /* runtime index: the table really is read through relocated pointers */
    neos_write("[hello] "); neos_write(greetings[k]); neos_write(" "); neos_write(greetings[k + 1]); neos_write(" "); neos_write(greetings[k + 2]);
    neos_write(" - pid "); put_dec(neos_getpid()); neos_write(", main at "); put_hex((u64)&main); neos_write("\n");
    neos_write("[hello] args:");
    for (int i = 0; i < argc; i++) { neos_write(" '"); neos_write(argv[i]); neos_write("'"); }
    neos_write("\n");
    const char *w = getenv_(envp, "NEOS_WORLD"), *home = getenv_(envp, "HOME"), *path = getenv_(envp, "PATH");
    neos_write("[hello] world="); neos_write(w ? w : "?"); neos_write("\n");
    neos_write("[hello] HOME="); neos_write(home ? home : "(unset)"); neos_write("\n");
    neos_write("[hello] PATH="); neos_write(path ? path : "(unset)"); neos_write("\n");

    volatile double x = 1.0;                                    /* SSE state must survive preemption */
    for (int i = 1; i <= 10; i++) x = x * 1.5 + i;
    neos_write("[hello] float check: "); put_dec((i64)x); neos_write("\n");

    char *m = neos_mmap(8192);
    if (m) { m[0] = 'A'; m[8191] = 'Z'; neos_write("[hello] mmap ok: "); char t[3] = { m[0], m[8191], 0 }; neos_write(t); neos_write("\n"); }

    static char out[1024];
    i64 rc = neos_tool("sys.info", out, sizeof out);
    neos_write("[hello] tool sys.info rc="); put_dec(rc); neos_write(" "); out[60] = 0; neos_write(out); neos_write("...\n");
    rc = neos_tool("tune.set sched.quantum 7", out, sizeof out);
    neos_write("[hello] tool tune.set rc="); put_dec(rc); neos_write(" "); neos_write(out); neos_write("\n");

    u64 t0 = neos_time_ns(); neos_sleep_ms(20); u64 dt = (neos_time_ns() - t0) / 1000000;
    neos_write("[hello] slept ~"); put_dec((i64)dt); neos_write(" ms\n");
    return 42;
}
