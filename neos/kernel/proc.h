#pragma once
#include "kernel.h"
#include "vm.h"

enum { P_FREE, P_RUN, P_ZOMBIE, P_EXITED };
#define MAX_PROCS 16
struct agent;

struct proc {
    int pid, state, tid;
    char name[24];
    struct aspace as;
    struct agent *principal;       /* the process is a principal: its tool calls are gated by these capabilities */
    i64 exit_code;
    u64 entry, mmap_next, start_tick, syscalls;
    int world;                     /* NEOS_VIEW_LINUX / NEOS_VIEW_WINDOWS: which convention its environment uses */
};

void proc_init(void);
struct spawn_opts { int world, prio, ngrant; const char *grant[8]; };    /* grants: "cap" or "cap:scope", applied before the program runs */
int  proc_spawn(const char *path, int argc, char **argv, const struct spawn_opts *o, int *pid_out, const char **err);
struct proc *proc_current(void);
struct proc *proc_get(int slot);
struct proc *proc_by_pid(int pid);
int  proc_kill(int pid, i64 code);
int  proc_wait(int pid, i64 *code, int (*interrupt)(void));     /* blocks; interrupt() != 0 aborts the wait */
struct frame *proc_fault(struct frame *f);
struct frame *user_syscall(struct frame *f);
int  elf_load(struct proc *p, const u8 *img, size_t n, u64 *entry, u64 *phdr, int *phnum, const char **err);
