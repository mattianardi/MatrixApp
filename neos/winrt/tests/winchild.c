#include <windows.h>
#include <stdio.h>
/* reads stdin to EOF, prints it upper-cased with a prefix; exit code 5 */
int main(int argc, char **argv)
{
    char line[256];
    printf("CHILD started argc=%d\n", argc);
    while (fgets(line, sizeof line, stdin)) { for (char *p = line; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; printf("CHILD got: %s", line); }
    return 5;
}
