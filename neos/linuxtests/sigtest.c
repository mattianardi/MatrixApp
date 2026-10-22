#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <setjmp.h>

static volatile int got_usr1, got_alrm, got_chld, segv_hits;
static sigjmp_buf jb;
static void on_usr1(int s, siginfo_t *si, void *uc) { (void)uc; got_usr1 = s * 1000 + (si ? 1 : 0); }
static void on_alrm(int s) { (void)s; got_alrm++; }
static void on_chld(int s) { (void)s; got_chld++; }
static void on_segv(int s, siginfo_t *si, void *uc) { (void)s; (void)uc; segv_hits++; siglongjmp(jb, (int)(si->si_addr == (void *)0x10 ? 7 : 1)); }

int main(void)
{
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1; sa.sa_flags = SA_SIGINFO; sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    printf("SIG raise: handler ran, value %d\n", got_usr1);
    kill(getpid(), SIGUSR1);
    printf("SIG kill self: handler count ok %d\n", got_usr1 == 10001);

    sigset_t m, old; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, &old);
    got_usr1 = 0; raise(SIGUSR1);
    printf("SIG blocked: pending, handler not run yet (%d)\n", got_usr1);
    sigprocmask(SIG_SETMASK, &old, NULL);
    printf("SIG unblocked: handler ran (%d)\n", got_usr1 != 0);

    signal(SIGALRM, on_alrm);
    alarm(1);
    struct timespec ts = { 3, 0 }, rem = { 0, 0 };
    int r = nanosleep(&ts, &rem);
    printf("SIG alarm interrupted sleep: r=%d alarms=%d\n", r, got_alrm);

    sa.sa_handler = on_chld; sa.sa_flags = 0; sigaction(SIGCHLD, &sa, NULL);
    pid_t c = fork();
    if (!c) _exit(3);
    int st; waitpid(c, &st, 0);
    usleep(20000);
    printf("SIG child exit: status %d sigchld=%d\n", WEXITSTATUS(st), got_chld > 0);

    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigaction(SIGSEGV, &sa, NULL);
    int v = sigsetjmp(jb, 1);
    if (!v) { volatile int *p = (int *)0x10; *p = 1; }
    printf("SIG segv caught and recovered: code=%d hits=%d\n", v, segv_hits);

    volatile double acc = 1.5;
    sa.sa_sigaction = on_usr1; sa.sa_flags = SA_SIGINFO; sigaction(SIGUSR1, &sa, NULL);
    for (int i = 0; i < 5; i++) { acc = acc * 1.1 + 0.5; raise(SIGUSR1); }
    printf("SIG fpu state preserved: %.3f\n", acc);
    printf("SIG done\n");
    return 0;
}
