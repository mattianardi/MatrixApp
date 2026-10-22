#pragma once
#include "vfs.h"

enum { FT_NONE, FT_INODE, FT_PIPE_R, FT_PIPE_W, FT_TTY, FT_DIR };
#define O_ACCMODE 3
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_NOCTTY 0400
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000

struct pipe;
struct file {
    int type, refs, flags;
    i64 off;
    struct inode *ino;
    struct pipe *pipe;
};

#define MAX_FDS 128
struct fdtable { struct file *f[MAX_FDS]; u8 cloexec[MAX_FDS]; int refs; };

struct fdtable *fdt_new(void);
struct fdtable *fdt_clone(struct fdtable *t);                 /* fork: same open files, independent table */
void   fdt_release(struct fdtable *t);                        /* drop one reference; closes everything at zero */
struct file *fdt_get(struct fdtable *t, int fd);
int    fdt_install(struct fdtable *t, struct file *f, int minfd, int cloexec);   /* fd or -E_MFILE */
int    fdt_close(struct fdtable *t, int fd);
void   fdt_close_cloexec(struct fdtable *t);
struct file *file_new(int type, int flags);
void   file_get(struct file *f);
void   file_put(struct file *f);
int    pipe_create(struct file **r, struct file **w);
long   file_read(struct file *f, void *buf, size_t n);        /* may sleep; <0 = -errno */
long   file_write(struct file *f, const void *buf, size_t n);
int    file_poll_ready(struct file *f, int events);           /* 1 = ready */

/* terminal: cooked input fed by whoever owns the keyboard (the shell while a program is in the foreground) */
void   tty_init(void);
void   tty_feed(int key);
long   tty_read(void *buf, size_t n, int nonblock);
void   tty_write(const void *buf, size_t n);
int    tty_has_input(void);
