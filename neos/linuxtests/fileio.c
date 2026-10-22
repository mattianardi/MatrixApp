#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

int main(void)
{
    mkdir("/tmp/t", 0755);
    FILE *f = fopen("/tmp/t/a.txt", "w"); fprintf(f, "line one\nline two\n"); fclose(f);
    f = fopen("/tmp/t/a.txt", "a"); fputs("line three\n", f); fclose(f);
    char buf[128]; int fd = open("/tmp/t/a.txt", O_RDONLY); ssize_t n = read(fd, buf, sizeof buf - 1); buf[n] = 0;
    printf("read %zd bytes:\n%s", n, buf);
    printf("lseek end=%ld\n", (long)lseek(fd, 0, SEEK_END)); close(fd);
    struct stat st; stat("/tmp/t/a.txt", &st); printf("stat: size=%ld regular=%d mode=%o\n", (long)st.st_size, S_ISREG(st.st_mode), st.st_mode & 0777);
    rename("/tmp/t/a.txt", "/tmp/t/b.txt");
    f = fopen("/tmp/t/c.txt", "w"); fputs("c", f); fclose(f);
    DIR *d = opendir("/tmp/t"); struct dirent *e; int cnt = 0;
    while ((e = readdir(d))) if (e->d_name[0] != '.') { printf("entry: %s\n", e->d_name); cnt++; }
    closedir(d);
    printf("entries=%d\n", cnt);
    printf("open missing: %s\n", open("/tmp/t/none", O_RDONLY) < 0 ? strerror(errno) : "??");
    unlink("/tmp/t/b.txt"); unlink("/tmp/t/c.txt"); rmdir("/tmp/t");
    printf("cleanup: %s\n", access("/tmp/t", F_OK) ? "gone" : "still there");
    char cwd[64]; chdir("/tmp"); getcwd(cwd, sizeof cwd); printf("cwd=%s\n", cwd);
    FILE *u = fopen("/dev/urandom", "r"); unsigned char r[4]; fread(r, 1, 4, u); fclose(u);
    FILE *p = fopen("/proc/meminfo", "r"); char line[80]; fgets(line, sizeof line, p); printf("proc: %s", line); fclose(p);
    return 0;
}
