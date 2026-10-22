#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
    int fds[2]; pipe(fds);
    pid_t c = fork();
    if (c == 0) {
        close(fds[0]);
        const char *m = "message from the child through a pipe\n";
        write(fds[1], m, strlen(m)); close(fds[1]);
        _exit(3);
    }
    close(fds[1]);
    char buf[100]; ssize_t n = read(fds[0], buf, sizeof buf - 1); buf[n > 0 ? n : 0] = 0;
    int st; waitpid(c, &st, 0);
    printf("parent got: %schild exited normally=%d code=%d\n", buf, WIFEXITED(st), WEXITSTATUS(st));

    /* a pipeline: ls | wc via fork + exec of a separate program, dup2 for plumbing */
    int p2[2]; pipe(p2);
    pid_t w = fork();
    if (w == 0) { dup2(p2[0], 0); close(p2[0]); close(p2[1]); execl("/bin/busybox", "busybox", "wc", "-l", (char *)0); _exit(127); }
    pid_t l = fork();
    if (l == 0) { dup2(p2[1], 1); close(p2[0]); close(p2[1]); execl("/bin/busybox", "busybox", "ls", "/bin", (char *)0); _exit(127); }
    close(p2[0]); close(p2[1]);
    waitpid(l, &st, 0); waitpid(w, &st, 0);
    printf("pipeline done, last status %d\n", WEXITSTATUS(st));
    return 0;
}
