#pragma once
#include "kernel.h"

/* BSD-style sockets over the kernel TCP/UDP stack (IPv4 only), used by the Linux syscall layer. */
struct sock;
enum { SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2 };
struct sock *sock_new(int type);
void   sock_free(struct sock *s);
long   sock_connect(struct sock *s, u32 ip, u16 port);
long   sock_bind(struct sock *s, u32 ip, u16 port);
long   sock_listen(struct sock *s, int backlog);
long   sock_accept(struct sock *s, int nonblock, struct sock **out);
long   sock_send(struct sock *s, const void *buf, size_t n, int has_dst, u32 ip, u16 port);
long   sock_recv(struct sock *s, void *buf, size_t n, int nonblock, u32 *sip, u16 *sport);
int    sock_poll(struct sock *s, int events);
u32    sock_in_gen(struct sock *s);
long   sock_shutdown(struct sock *s, int how);
void   sock_names(struct sock *s, u32 *lip, u16 *lport, u32 *rip, u16 *rport);
int    sock_is_stream(struct sock *s);
int    sock_nread(struct sock *s);
void   sock_set_unix(struct sock *s, int v);                          /* AF_UNIX sockets ride on loopback ports (see linux.c) */
int    sock_is_unix(struct sock *s);
