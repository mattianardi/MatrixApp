/* winrt entry: winrt <program.exe> [args...]  - the kernel starts PE files this way automatically. */
#include "winrt.h"

void k32_init(void);

int main(int argc, char **argv)
{
    g_argc = argc; g_argv = argv;
    if (argc < 2) { fputs("usage: winrt program.exe [arguments]\n", stderr); return 2; }
    char *p = argv[1];
    char rp[4096];
    if (realpath(p, rp) == NULL) strlcpy(g_exe_path_lin, p, sizeof g_exe_path_lin); else strlcpy(g_exe_path_lin, rp, sizeof g_exe_path_lin);
    path_lin_to_win(g_exe_path_lin, g_exe_path_win, sizeof g_exe_path_win);
    teb_init_thread(NULL);
    k32_init();
    Module *m = pe_load(g_exe_path_lin, argv[1], 0);
    if (!m) { fprintf(stderr, "winrt: cannot open %s\n", argv[1]); return 126; }
    g_main_module = m;
    int rc = pe_call_entry(m, 1);
    fflush(NULL);
    return rc;
}
