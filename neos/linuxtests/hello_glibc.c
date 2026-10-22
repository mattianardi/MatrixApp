#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(int argc, char **argv, char **envp)
{
    struct utsname u; uname(&u);
    printf("hello from an unmodified Linux binary (glibc, static) on %s %s\n", u.sysname, u.release);
    printf("argc=%d pid=%d\n", argc, (int)getpid());
    for (int i = 0; i < argc; i++) printf("  argv[%d]='%s' (%p)\n", i, argv[i], (void *)argv[i]);
    int n = 0; for (char **e = envp; *e; e++) n++;
    printf("env vars: %d, HOME=%s NEOS_WORLD=%s\n", n, getenv("HOME") ? getenv("HOME") : "(unset)", getenv("NEOS_WORLD") ? getenv("NEOS_WORLD") : "(unset)");
    int v[] = { 5, 3, 9, 1, 7 }; qsort(v, 5, sizeof v[0], cmp);
    printf("qsort: %d %d %d %d %d\n", v[0], v[1], v[2], v[3], v[4]);
    char *big = malloc(5 << 20); memset(big, 'x', 5 << 20); big[(5 << 20) - 1] = 0;
    printf("malloc 5 MiB ok, strlen=%zu\n", strlen(big)); free(big);
    double x = 1.0; for (int i = 1; i <= 10; i++) x = x * 1.5 + i;
    printf("float: %.3f\n", x);
    time_t t = time(NULL); struct tm *tm = gmtime(&t);
    printf("clock: year %d (wall clock from the RTC)\n", tm->tm_year + 1900);
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); usleep(20000); clock_gettime(CLOCK_MONOTONIC, &b);
    printf("slept %ld ms\n", (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
    fprintf(stderr, "this line goes to stderr\n");
    return 7;
}
