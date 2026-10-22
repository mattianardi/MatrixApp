#include "kernel.h"

#define MAX_INFO 24
static struct { const char *label; void (*fmt)(char *, size_t); } info[MAX_INFO];
static int ninfo;

void splash_info(const char *label, void (*fmt)(char *, size_t))
{
    if (ninfo < MAX_INFO) info[ninfo++] = (typeof(info[0])){ label, fmt };
}

void splash_show(void)
{
    static const char *logo[5] = {
        " _   _ _____  ___  ____  ",
        "| \\ | | ____|/ _ \\/ ___| ",
        "|  \\| |  _| | | | \\___ \\ ",
        "| |\\  | |___| |_| |___) |",
        "|_| \\_|_____|\\___/|____/ ",
    };
    static const char *grad[5] = { "\x1b[96m", "\x1b[96m", "\x1b[36m", "\x1b[94m", "\x1b[34m" };
    kprintf("\n");
    for (int i = 0; i < 5; i++) {
        kprintf("  %s%s\x1b[0m", grad[i], logo[i]);
        if (i == 1) kprintf("   \x1b[1mReal-Time Operating System\x1b[0m");
        if (i == 2) kprintf("   \x1b[90mby\x1b[0m \x1b[1;97m%s\x1b[0m", NEOS_AUTHOR);
        if (i == 3) kprintf("   \x1b[90mversion %s\x1b[0m", NEOS_VERSION);
        kprintf("\n");
    }
    kprintf("\n");
    for (int i = 0; i < ninfo; i++) {
        char v[96];
        info[i].fmt(v, sizeof v);
        kprintf("  \x1b[36m%-9s\x1b[0m %s\n", info[i].label, v);
    }
    kprintf("\n");
}
