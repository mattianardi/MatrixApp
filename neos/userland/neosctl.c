/* neosctl: call any NEOS tool (the same capability-gated tools AI agents use) from a shell script or any program.
 *   neosctl tune.get sched.quantum        neosctl pkg.record pip:numpy 2.1 pip        neosctl threads
 * Prints the JSON the tool returns; exit status = the tool's result code (0 = ok). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: neosctl <tool> [args...]   (neosctl tools lists them)\n"); return 2; }
    char cmd[512]; size_t l = 0;
    for (int i = 1; i < argc; i++) {
        int sp = strchr(argv[i], ' ') != NULL;
        size_t need = strlen(argv[i]) + (sp ? 2 : 0) + 1;
        if (l + need + 1 >= sizeof cmd) { fprintf(stderr, "neosctl: command too long\n"); return 2; }
        if (l) cmd[l++] = ' ';
        if (sp) cmd[l++] = '"';
        memcpy(cmd + l, argv[i], strlen(argv[i])); l += strlen(argv[i]);
        if (sp) cmd[l++] = '"';
    }
    cmd[l] = 0;
    char out[8192];
    long rc = syscall(1000, cmd, out, sizeof out);
    if (out[0]) { fputs(out, rc ? stderr : stdout); if (out[strlen(out) - 1] != '\n') fputc('\n', rc ? stderr : stdout); }
    return rc < 0 ? 1 : (int)rc > 255 ? 1 : (int)rc;
}
