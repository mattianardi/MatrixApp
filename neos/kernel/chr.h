#pragma once
#include "kernel.h"

/* Character devices for the Linux layer: /dev/fb0, /dev/input/event*, ... Each registers a small operations table and gets a /dev node;
 * opening it yields a file of type FT_CHR that dispatches here. */
#define DEV_CHR0 200
#define CHR_MAX 32
struct proc;
struct chrops {
    long (*read)(int minor, void *buf, size_t n, int nonblock);
    long (*write)(int minor, const void *buf, size_t n);
    int  (*poll)(int minor, int events);
    long (*ioctl)(int minor, u64 req, u64 arg, struct proc *p);
    u32  (*gen)(int minor);                                         /* grows when input arrives (edge-triggered epoll) */
    int  (*open)(int minor);                                        /* <0: refuse */
    void (*close)(int minor);
    int  (*mmap)(int minor, struct proc *p, u64 va, u64 len, int prot, u64 off);    /* map the device into p's address space at va */
    u64  (*size)(int minor);
    int  per_open;                                                  /* open() returns a handle that replaces the minor in every later call (one object per open file) */
};
int chr_register(const char *path, const struct chrops *ops, int minor);
const struct chrops *chr_ops(int slot);
int chr_minor(int slot);
