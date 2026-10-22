#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <spawn.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
extern char **environ;
int main(void)
{
    char *av[] = { "/bin/neosctl", "tune.get", "sched.quantum", NULL };
    pid_t pid; int st;
    int r = posix_spawn(&pid, "/bin/neosctl", NULL, NULL, av, environ);
    printf("SPAWN posix_spawn r=%d pid=%d\n", r, (int)pid); fflush(stdout);
    waitpid(pid, &st, 0); printf("SPAWN posix_spawn status=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    pid = vfork();
    if (pid == 0) { execv("/bin/neosctl", av); _exit(126); }
    waitpid(pid, &st, 0); printf("SPAWN vfork+exec status=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    pid = fork();
    if (pid == 0) { execv("/bin/neosctl", av); _exit(126); }
    waitpid(pid, &st, 0); printf("SPAWN fork+exec status=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
