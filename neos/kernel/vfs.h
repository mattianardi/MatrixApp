#pragma once
#include "kernel.h"

/* Linux errno values (returned negated by the Linux ABI) */
#define E_PERM 1
#define E_NOENT 2
#define E_SRCH 3
#define E_INTR 4
#define E_IO 5
#define E_BADF 9
#define E_CHILD 10
#define E_AGAIN 11
#define E_NOMEM 12
#define E_ACCES 13
#define E_FAULT 14
#define E_EXIST 17
#define E_NOTDIR 20
#define E_ISDIR 21
#define E_INVAL 22
#define E_MFILE 24
#define E_NOTTY 25
#define E_SPIPE 29
#define E_PIPE 32
#define E_RANGE 34
#define E_NAMETOOLONG 36
#define E_NOSYS 38
#define E_NOTEMPTY 39
#define E_LOOP 40

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_IFLNK  0120000

enum { DEV_NONE, DEV_NULL, DEV_ZERO, DEV_RANDOM, DEV_TTY };

struct inode {
    int used;
    char path[256];
    u32 mode, ino;
    u8 *data;
    size_t size, cap;
    u64 mtime;               /* seconds since boot-relative epoch (see vfs_now) */
    int dev;
    char link[100];
};

/* path helpers */
void fs_norm_at(const char *cwd, const char *in, char *out, size_t n);
/* inode access (absolute normalized paths) */
struct inode *vfs_lookup(const char *path);
int  vfs_create(const char *path, u32 mode, struct inode **out);     /* creates parents; -E_* on error */
int  vfs_mkdir(const char *path, u32 mode);
int  vfs_unlink(const char *path);                                   /* files and symlinks */
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *from, const char *to);
int  vfs_symlink(const char *target, const char *path);
long inode_read(struct inode *i, u64 off, void *buf, size_t n);
long inode_write(struct inode *i, u64 off, const void *buf, size_t n);
int  inode_truncate(struct inode *i, u64 len);
int  vfs_readdir(const char *dir, int idx, char *name, size_t nn, int *type, u32 *ino);   /* 0 = entry returned */
u64  vfs_now(void);                                                  /* unix seconds */
void vfs_walk(void (*cb)(struct inode *i, void *ctx), void *ctx);
void vfs_register_proc_hook(int (*hook)(const char *path, char *buf, size_t n));       /* synthetic /proc files */
