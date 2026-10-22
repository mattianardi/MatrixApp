#include "neos.h"

/* Misbehaviour lab: every mode must kill only this process. */
static void bad_buf(void)
{
    i64 r = neos_sys(USYS_WRITE, 1, 0xFFFF800000100000ul, 16, 0);
    neos_write("[crash] write(kernel address) -> "); put_dec(r); neos_write("\n");
    r = neos_sys(USYS_WRITE, 1, 0x1000, 16, 0);
    neos_write("[crash] write(unmapped) -> "); put_dec(r); neos_write("\n");
    r = neos_sys(USYS_TOOL, 0xFFFF800000100000ul, 0x1000, 64, 0);
    neos_write("[crash] tool(kernel cmd pointer) -> "); put_dec(r); neos_write("\n");
    r = neos_sys(9999, 0, 0, 0, 0);
    neos_write("[crash] unknown syscall -> "); put_dec(r); neos_write("\n");
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *m = argc > 1 ? argv[1] : "badbuf";
    neos_write("[crash] mode "); neos_write(m); neos_write("\n");
    if (m[0] == 'b') { bad_buf(); return 0; }
    if (m[0] == 'k') *(volatile int *)0xFFFF800000000000ul = 1;                   /* write kernel memory */
    if (m[0] == 'K') return *(volatile int *)0xFFFFFFFF80100000ul;                /* read kernel text    */
    if (m[0] == 'n') return *(volatile int *)0;                                   /* null pointer        */
    if (m[0] == 'c') __asm__ volatile("cli");                                     /* privileged instruction */
    if (m[0] == 'i') __asm__ volatile("inb $0x60, %%al" ::: "al");                /* port I/O            */
    if (m[0] == 'e') { static char data[16] = { (char)0xC3 }; __asm__ volatile("call *%0" :: "r"(data) : "memory"); }   /* execute data (NX) */
    if (m[0] == 'd') { volatile int z = 0; return 5 / z; }                        /* divide by zero      */
    if (m[0] == 'm') { for (int i = 0; i < 100000; i++) if (!neos_mmap(4096 * 1000)) { neos_write("[crash] mmap refused at "); put_dec(i); neos_write("\n"); return 7; } }
    neos_write("[crash] survived?!\n");
    return 1;
}
