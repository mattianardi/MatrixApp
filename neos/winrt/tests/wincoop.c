#include <windows.h>
#include <stdio.h>
#include <string.h>
/* launches winchild.exe with a pipe on its stdin/stdout, feeds it text, reads the answer back */
int main(void)
{
    HANDLE inR, inW, outR, outW; SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    CreatePipe(&inR, &inW, &sa, 0); CreatePipe(&outR, &outW, &sa, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = outW;
    PROCESS_INFORMATION pi;
    char cmd[] = "winchild.exe alpha";
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("COOP createprocess failed %lu\n", (unsigned long)GetLastError()); return 1; }
    CloseHandle(inR); CloseHandle(outW);
    DWORD w; WriteFile(inW, "hello neos\nsecond line\n", 23, &w, NULL); CloseHandle(inW);
    char buf[512]; DWORD r, total = 0;
    while (ReadFile(outR, buf + total, (DWORD)(sizeof buf - 1 - total), &r, NULL) && r) total += r;
    buf[total] = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code; GetExitCodeProcess(pi.hProcess, &code);
    printf("COOP child output:\n%s", buf);
    printf("COOP child exit code %lu\n", (unsigned long)code);
    return 0;
}
