#include "neos.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    volatile unsigned long n = 0;
    double acc = 0.5;
    for (;;) { n++; acc = acc * 1.0000001 + 0.000001; if ((n & 0xFFFFFFF) == 0) neos_write("."); }
}
