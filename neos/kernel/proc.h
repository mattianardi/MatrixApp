#pragma once
#include "kernel.h"
#include "vm.h"
#include "fdt.h"

enum { P_FREE, P_BUILD, P_RUN, P_ZOMBIE, P_EXITED };       /* P_BUILD: slot reserved while the process is being created */
#define MAX_PROCS 32
struct agent;

struct sigact { u64 handler, flags, mask, restorer; };

struct proc {
    int pid, ppid, state, collected;
    char name[24], exe[128], cwd[96];
    struct aspace as;
    struct fdtable *fds;
    struct agent *principal;       /* every process is a principal: its tool calls are gated by these capabilities */
    int exit_status;               /* Linux wait status */
    i64 exit_code;                 /* what the shell shows: exit code, or 128+signal */
    u64 entry, brk_start, brk, mmap_next, start_tick, syscalls;
    int world;                     /* NEOS_VIEW_LINUX / NEOS_VIEW_WINDOWS: which convention its environment uses */
    int nthreads, main_tid, next_utid;
    struct sigact sig[65];
};

struct spawn_opts { int world, prio, ngrant; const char *grant[8]; };    /* grants: "cap" or "cap:scope", applied before the program runs */

void proc_init(void);
int  proc_spawn(const char *path, int argc, char **argv, const struct spawn_opts *o, int *pid_out, const char **err);
struct proc *proc_current(void);
struct proc *proc_get(int slot);
struct proc *proc_by_pid(int pid);
int  proc_kill(int pid, i64 code);                 /* shell convention: terminate with this exit code */
int  proc_signal(int pid, int sig);                /* Linux kill(2) semantics (default actions only) */
int  proc_wait(int pid, i64 *code, int (*interrupt)(void));     /* shell: blocks until the process ends */
struct frame *proc_fault(struct frame *f);
struct frame *user_syscall(struct frame *f);       /* native NEOS ABI (int 0x80) */
struct frame *linux_syscall(struct frame *f);      /* Linux x86-64 ABI (syscall instruction) */

/* loading / lifecycle used by the Linux layer */
struct argblock { char *pool; size_t used, cap; char **argv, **envp; int argc, envc; };
struct argblock *argblock_new(void);
void argblock_free(struct argblock *a);
int  argblock_add(struct argblock *a, int env, const char *s);
int  proc_execve(struct proc *p, struct frame *f, const char *path, struct argblock *ab, const char **err);
long proc_fork(struct proc *p, const struct frame *f, u64 child_stack);
long proc_clone_thread(struct proc *p, const struct frame *f, u64 child_stack, u64 tls, u64 ptid, u64 ctid, u64 flags);
struct frame *proc_exit_thread(struct proc *p, struct frame *f, int status);
struct frame *proc_exit_group(struct proc *p, struct frame *f, int status);
long proc_wait4(struct proc *p, int pid, u64 ustatus, int options);

/* elf.c */
struct elf_info { u64 entry, phdr, base, top, interp_base; int phnum; char interp[128]; };
int  elf_load(struct aspace *as, const u8 *img, size_t n, u64 forced_bias, struct elf_info *out, const char **err);
void sched_set_fsbase(u64 v);

/* binfmt registry (extension -> interpreter) */
int  binfmt_add(const char *ext, const char *interp, const char *arg);
int  binfmt_remove(const char *ext);
int  binfmt_get(int i, char *ext, char *interp, char *arg);
