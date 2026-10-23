/* winrt extension test: registry, Winsock, SRW locks, condition variables, one-time init, multimedia timer, COM start-up. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static SRWLOCK srw = SRWLOCK_INIT;
static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static CRITICAL_SECTION cs;
static int ready, shared_value;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static int once_runs;
static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; once_runs++; return TRUE; }

static DWORD WINAPI producer(LPVOID p)
{
    (void)p; Sleep(20);
    AcquireSRWLockExclusive(&srw); shared_value = 42; ready = 1; ReleaseSRWLockExclusive(&srw);
    EnterCriticalSection(&cs); WakeAllConditionVariable(&cv); LeaveCriticalSection(&cs);
    return 0;
}
static DWORD WINAPI echo_server(LPVOID p)
{
    SOCKET ls = (SOCKET)(ULONG_PTR)p; struct sockaddr_in a; int al = sizeof a;
    SOCKET c = accept(ls, (struct sockaddr *)&a, &al); char buf[64]; int n = recv(c, buf, sizeof buf, 0);
    for (int i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    send(c, buf, n, 0); closesocket(c); return 0;
}

int main(void)
{
    /* registry */
    HKEY k; DWORD disp = 0;
    LONG r = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\NeosTest\\Sub", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp);
    printf("EXT reg create=%ld disp=%lu\n", r, (unsigned long)disp);
    RegSetValueExA(k, "Name", 0, REG_SZ, (const BYTE *)"hello registry", 15);
    DWORD num = 1234; RegSetValueExA(k, "Count", 0, REG_DWORD, (const BYTE *)&num, 4);
    char buf[64]; DWORD len = sizeof buf, type = 0;
    r = RegQueryValueExA(k, "Name", NULL, &type, (BYTE *)buf, &len);
    printf("EXT reg read=%ld type=%lu value=%s\n", r, (unsigned long)type, buf);
    DWORD got = 0; len = 4; RegQueryValueExA(k, "Count", NULL, &type, (BYTE *)&got, &len); printf("EXT reg dword=%lu\n", (unsigned long)got);
    len = sizeof buf; r = RegQueryValueExA(k, "Missing", NULL, &type, (BYTE *)buf, &len); printf("EXT reg missing=%ld\n", r);
    int cnt = 0; char nm[64]; DWORD nl;
    for (DWORD i = 0;; i++) { nl = sizeof nm; if (RegEnumValueA(k, i, nm, &nl, NULL, NULL, NULL, NULL)) break; cnt++; }
    printf("EXT reg values=%d\n", cnt);
    RegCloseKey(k);
    r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\NeosTest", 0, KEY_READ, &k);
    nl = sizeof nm; r = RegEnumKeyExA(k, 0, nm, &nl, NULL, NULL, NULL, NULL); printf("EXT reg subkey=%s\n", r ? "(none)" : nm);
    RegCloseKey(k);
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\NeosTest\\Sub");
    r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\NeosTest\\Sub", 0, KEY_READ, &k); printf("EXT reg deleted=%s\n", r == ERROR_FILE_NOT_FOUND ? "yes" : "no");
    len = sizeof buf; r = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentVersion", RRF_RT_REG_SZ, NULL, buf, &len);
    printf("EXT reg system=%ld %s\n", r, r ? "" : buf);

    /* Winsock loopback TCP echo */
    WSADATA wsa; printf("EXT wsa=%d\n", WSAStartup(MAKEWORD(2, 2), &wsa));
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = inet_addr("127.0.0.1"); sa.sin_port = 0;
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    bind(ls, (struct sockaddr *)&sa, sizeof sa); listen(ls, 4);
    int sl = sizeof sa; getsockname(ls, (struct sockaddr *)&sa, &sl);
    HANDLE srv = CreateThread(NULL, 0, echo_server, (LPVOID)(ULONG_PTR)ls, 0, NULL);
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int rc = connect(c, (struct sockaddr *)&sa, sizeof sa);
    send(c, "winsock works", 13, 0);
    char rb[64] = { 0 }; int n = recv(c, rb, sizeof rb - 1, 0);
    printf("EXT sock connect=%d recv=%d '%s'\n", rc, n, rb);
    closesocket(c); WaitForSingleObject(srv, 5000); closesocket(ls);
    SOCKET dead = socket(AF_INET, SOCK_STREAM, 0); sa.sin_port = htons(9);
    rc = connect(dead, (struct sockaddr *)&sa, sizeof sa); printf("EXT sock refused=%d err=%d\n", rc, WSAGetLastError()); closesocket(dead);
    printf("EXT htons=%u inet_ntoa=%s\n", htons(0x1234), inet_ntoa(sa.sin_addr));
    WSACleanup();

    /* SRW lock + condition variable + one-time init */
    InitializeCriticalSection(&cs);
    HANDLE pt = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    EnterCriticalSection(&cs);
    while (!ready) SleepConditionVariableCS(&cv, &cs, 5000);
    LeaveCriticalSection(&cs);
    AcquireSRWLockShared(&srw); int v = shared_value; ReleaseSRWLockShared(&srw);
    WaitForSingleObject(pt, 5000);
    printf("EXT condvar value=%d\n", v);
    InitOnceExecuteOnce(&once, once_fn, NULL, NULL); InitOnceExecuteOnce(&once, once_fn, NULL, NULL);
    printf("EXT once runs=%d\n", once_runs);

    /* timers, COM, misc */
    DWORD t0 = timeGetTime(); Sleep(30); DWORD dt = timeGetTime() - t0;
    printf("EXT timer ok=%d\n", dt >= 25 && dt < 500);
    printf("EXT com init=%ld\n", (long)CoInitializeEx(NULL, COINIT_MULTITHREADED));
    void *obj = NULL; HRESULT hr = CoCreateInstance(&CLSID_NULL, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, &obj);
    printf("EXT com create=%s\n", hr == REGDB_E_CLASSNOTREG ? "REGDB_E_CLASSNOTREG" : "other");
    CoUninitialize();
    char sd[260]; GetSystemDirectoryA(sd, sizeof sd); printf("EXT sysdir=%s\n", sd);
    LONGLONG ll = 5; InterlockedIncrement64(&ll); printf("EXT interlocked64=%lld\n", ll);
    printf("EXT done\n");
    return 0;
}
