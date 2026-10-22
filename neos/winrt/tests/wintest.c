#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static DWORD WINAPI worker(LPVOID p) { InterlockedIncrement((LONG *)p); return 7; }
static __thread int tl = 5;

int main(int argc, char **argv)
{
    printf("WIN argc=%d argv0=%s\n", argc, argv[0]);
    for (int i = 1; i < argc; i++) printf("WIN arg%d=%s\n", i, argv[i]);
    printf("WIN fmt %5.2f|%-6d|%08x|%s|%c|%lld|%zu|%ls\n", 3.14159, 42, 0xBEEF, "str", 'z', 1234567890123LL, (size_t)99, L"wide");
    CreateDirectoryA("C:\\shared", NULL);
    HANDLE f = CreateFileA("C:\\shared\\win.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0; WriteFile(f, "from-windows\n", 13, &w, NULL); CloseHandle(f);
    printf("WIN wrote %lu\n", (unsigned long)w);
    f = CreateFileA("C:\\shared\\win.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    char buf[64] = {0}; DWORD r = 0; ReadFile(f, buf, 63, &r, NULL); CloseHandle(f);
    printf("WIN read %lu: %s", (unsigned long)r, buf);
    FILE *fp = fopen("C:\\shared\\crt.txt", "w"); fprintf(fp, "crt line %d\n", 9); fclose(fp);
    char cwd[260]; GetCurrentDirectoryA(260, cwd); printf("WIN cwd=%s\n", cwd);
    LONG counter = 0; HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, &counter, 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    DWORD code = 0; GetExitCodeThread(th[0], &code);
    printf("WIN threads counter=%ld exit=%lu tls=%d\n", (long)counter, (unsigned long)code, tl);
    CRITICAL_SECTION cs; InitializeCriticalSection(&cs); EnterCriticalSection(&cs); LeaveCriticalSection(&cs); DeleteCriticalSection(&cs);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA("C:\\shared\\*.txt", &fd); int n = 0;
    if (h != INVALID_HANDLE_VALUE) { do n++; while (FindNextFileA(h, &fd)); FindClose(h); }
    printf("WIN found %d txt files\n", n);
    char *e = getenv("NEOS_X"); printf("WIN env NEOS_X=%s\n", e ? e : "(unset)");
    void *m = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); memset(m, 1, 8192); VirtualFree(m, 0, MEM_RELEASE);
    qsort(&counter, 0, 4, NULL);
    printf("WIN done\n");
    return 3;
}
