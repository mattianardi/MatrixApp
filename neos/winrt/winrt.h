/* winrt - the NEOS Windows runtime: loads 64-bit PE programs and DLLs and provides Win32 / msvcrt on top of Linux-ABI syscalls.
 * Windows paths (C:\dir\f.txt) map to /mnt/c/dir/f.txt - the very same files a Linux program sees; the environment comes from
 * the shared variable bus in its Windows form. */
#pragma once
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

typedef uint8_t BYTE; typedef uint16_t WORD; typedef uint32_t DWORD; typedef int32_t LONG; typedef uint32_t ULONG; typedef int BOOL;
typedef uint64_t QWORD; typedef int64_t LONGLONG; typedef uint64_t ULONGLONG; typedef uint64_t ULONG_PTR; typedef int64_t LONG_PTR;
typedef void *HANDLE; typedef void *HMODULE; typedef void *LPVOID; typedef const void *LPCVOID; typedef char *LPSTR; typedef const char *LPCSTR;
typedef uint16_t WCHAR; typedef WCHAR *LPWSTR; typedef const WCHAR *LPCWSTR; typedef DWORD *LPDWORD; typedef void *FARPROC;
#define WINAPI __attribute__((ms_abi))
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* ---- PE structures ---- */
#pragma pack(push, 1)
typedef struct { WORD e_magic; BYTE pad[58]; DWORD e_lfanew; } DosHeader;
typedef struct { WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; } FileHeader;
typedef struct { DWORD VirtualAddress, Size; } DataDir;
typedef struct {
    WORD Magic; BYTE MajorLinker, MinorLinker; DWORD SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode;
    QWORD ImageBase; DWORD SectionAlignment, FileAlignment; WORD MajorOS, MinorOS, MajorImage, MinorImage, MajorSub, MinorSub; DWORD Win32Version, SizeOfImage, SizeOfHeaders, CheckSum;
    WORD Subsystem, DllCharacteristics; QWORD StackReserve, StackCommit, HeapReserve, HeapCommit; DWORD LoaderFlags, NumberOfRvaAndSizes; DataDir Dir[16];
} OptHeader;
typedef struct { DWORD Signature; FileHeader File; OptHeader Opt; } NtHeaders;
typedef struct { char Name[8]; DWORD VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData, PtrReloc, PtrLine; WORD NReloc, NLine; DWORD Characteristics; } Section;
typedef struct { DWORD OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } ImportDesc;
typedef struct { DWORD Characteristics, TimeDateStamp; WORD MajorVersion, MinorVersion; DWORD Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals; } ExportDir;
typedef struct { DWORD VirtualAddress, SizeOfBlock; } RelocBlock;
typedef struct { QWORD StartRaw, EndRaw, AddressOfIndex, AddressOfCallBacks; DWORD SizeOfZeroFill, Characteristics; } TlsDir;
#pragma pack(pop)

typedef struct Module {
    char name[64];
    uint8_t *base; size_t size; NtHeaders *nt;
    int is_dll, loaded;
    QWORD entry;
    struct Module *next;
} Module;

/* ---- winrt internals shared across files ---- */
extern Module *g_main_module;
extern char g_exe_path_win[260], g_exe_path_lin[260];
extern int g_argc; extern char **g_argv;

void rt_die(const char *fmt, ...) __attribute__((noreturn));
void *rt_resolve(const char *dll, const char *name);          /* built-in shim symbol or NULL */
Module *pe_load(const char *path_lin, const char *name, int is_dll);
void *pe_export(Module *m, const char *name);
void *pe_resolve_import(const char *dll, const char *name, int ordinal);
int  pe_call_entry(Module *m, int reason);

/* paths */
char *path_win_to_lin(const char *w, char *out, size_t n);
char *path_lin_to_win(const char *l, char *out, size_t n);
/* TEB */
void teb_init_thread(void *stack_base);
DWORD *teb_last_error(void);
void set_last_error(DWORD e);
DWORD errno_to_win(int e);
