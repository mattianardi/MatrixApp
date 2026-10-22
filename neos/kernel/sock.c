#include "sock.h"
#include "net.h"
#include "vfs.h"

#define DGRAMS 16
#define DG_MAX 1472
struct dgram { u32 src; u16 sport, len; u8 data[DG_MAX]; };
struct sock {
    int type;
    struct tcp_conn *tc; struct tcp_listener *ls;
    u32 lip, rip; u16 lport, rport;
    u8 bound, connected, shut_rd, shut_wr;
    struct dgram *dq; volatile u32 dh, dt;
};

struct sock *sock_new(int type)
{
    struct sock *s = kmalloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->type = type;
    if (type == SOCK_DGRAM_) { s->dq = kmalloc(DGRAMS * sizeof(struct dgram)); if (!s->dq) { kfree(s); return NULL; } }
    return s;
}

void sock_free(struct sock *s)
{
    if (s->tc) tcp_close(s->tc);
    if (s->ls) tcp_unlisten(s->ls);
    if (s->type == SOCK_DGRAM_ && s->lport) udp_unbind(s->lport);
    if (s->dq) kfree(s->dq);
    kfree(s);
}

static void udp_rx(void *ctx, u32 src, u16 sport, const u8 *d, int len)
{
    struct sock *s = ctx;
    if (s->dh - s->dt >= DGRAMS || len > DG_MAX) return;
    struct dgram *g = &s->dq[s->dh % DGRAMS];
    g->src = src; g->sport = sport; g->len = (u16)len; memcpy(g->data, d, (size_t)len);
    s->dh++;
}

static long ensure_udp_bound(struct sock *s)
{
    if (s->bound) return 0;
    for (int i = 0; i < 64; i++) { u16 p = udp_ephemeral(); if (!udp_bind_ctx(p, udp_rx, s)) { s->lport = p; s->bound = 1; return 0; } }
    return -98;                                                        /* EADDRINUSE */
}

long sock_bind(struct sock *s, u32 ip, u16 port)
{
    if (s->bound) return -22;
    s->lip = ip;
    if (s->type == SOCK_DGRAM_) {
        if (!port) return ensure_udp_bound(s);
        if (udp_bind_ctx(port, udp_rx, s)) return -98;
        s->lport = port; s->bound = 1; return 0;
    }
    s->lport = port; s->bound = 1;                                      /* TCP: remembered until listen() */
    return 0;
}

long sock_listen(struct sock *s, int backlog)
{
    (void)backlog;
    if (s->type != SOCK_STREAM_ || s->tc) return -95;
    if (!s->bound || !s->lport) { static u16 np = 30000; s->lport = np++; s->bound = 1; }
    s->ls = tcp_listen(s->lport);
    return s->ls ? 0 : -98;
}

long sock_connect(struct sock *s, u32 ip, u16 port)
{
    if (s->type == SOCK_DGRAM_) { long r = ensure_udp_bound(s); if (r) return r; s->rip = ip; s->rport = port; s->connected = 1; return 0; }
    if (s->tc) return -106;                                             /* EISCONN */
    if (!ip) ip = 0x7F000001;
    s->tc = tcp_connect(ip, port, 8000);
    if (!s->tc) return -111;                                            /* ECONNREFUSED */
    u32 pi; u16 pp, lp; tcp_peer(s->tc, &pi, &pp, &lp); s->rip = ip; s->rport = port; s->lport = lp; s->connected = 1;
    return 0;
}

long sock_accept(struct sock *s, int nonblock, struct sock **out)
{
    if (!s->ls) return -22;
    struct tcp_conn *c;
    while (!(c = tcp_accept_try(s->ls))) {
        if (nonblock) return -11;
        thread_sleep_until(ticks + 1);
    }
    struct sock *n = sock_new(SOCK_STREAM_);
    if (!n) { tcp_close(c); return -12; }
    u16 lp; tcp_peer(c, &n->rip, &n->rport, &lp); n->tc = c; n->lport = lp; n->lip = s->lip; n->connected = 1; n->bound = 1;
    *out = n;
    return 0;
}

long sock_send(struct sock *s, const void *buf, size_t n, int has_dst, u32 ip, u16 port)
{
    if (s->type == SOCK_DGRAM_) {
        long r = ensure_udp_bound(s); if (r) return r;
        if (!has_dst) { if (!s->connected) return -89; ip = s->rip; port = s->rport; }
        if (n > 1400) return -90;                                       /* EMSGSIZE */
        if (!ip) ip = 0x7F000001;
        return udp_send(ip, s->lport, port, buf, (int)n) ? -101 : (long)n;      /* ENETUNREACH */
    }
    if (!s->tc || s->shut_wr) return -32;                               /* EPIPE */
    if (!tcp_is_open(s->tc) && !tcp_available(s->tc)) return -32;
    if (!n) return 0;
    return tcp_send(s->tc, buf, (int)n) ? -32 : (long)n;
}

long sock_recv(struct sock *s, void *buf, size_t n, int nonblock, u32 *sip, u16 *sport)
{
    if (s->type == SOCK_DGRAM_) {
        while (s->dh == s->dt) { if (nonblock) return -11; thread_sleep_until(ticks + 1); }
        struct dgram *g = &s->dq[s->dt % DGRAMS];
        size_t c = g->len < n ? g->len : n;
        memcpy(buf, g->data, c);
        if (sip) *sip = g->src;
        if (sport) *sport = g->sport;
        s->dt++;
        return (long)c;
    }
    if (!s->tc) return -107;                                            /* ENOTCONN */
    if (s->shut_rd) return 0;
    while (!tcp_available(s->tc)) { if (nonblock) return -11; thread_sleep_until(ticks + 1); }
    int r = tcp_recv(s->tc, buf, (int)n, 0);
    if (sip) *sip = s->rip;
    if (sport) *sport = s->rport;
    return r < 0 ? (r == -2 ? -104 : -11) : r;                          /* ECONNRESET */
}

int sock_poll(struct sock *s, int events)
{
    int r = 0;
    if (s->type == SOCK_DGRAM_) { if ((events & 1) && s->dh != s->dt) r |= 1; if (events & 4) r |= 4; return r; }
    if (s->ls) { if ((events & 1) && tcp_listener_ready(s->ls)) r |= 1; return r; }
    if (!s->tc) { if (events & 4) r |= 4; return r; }
    if ((events & 1) && tcp_available(s->tc)) r |= 1;
    if ((events & 4) && tcp_is_open(s->tc)) r |= 4;
    if (!tcp_is_open(s->tc) && !tcp_available(s->tc)) r |= 0x10;       /* POLLHUP */
    return r;
}

/* monotonic "something arrived" counter, so edge-triggered epoll can tell a new arrival from old unread data */
u32 sock_in_gen(struct sock *s)
{
    if (s->type == SOCK_DGRAM_) return s->dh;
    if (s->ls) return tcp_listener_gen(s->ls);
    if (s->tc) return tcp_rx_gen(s->tc);
    return 0;
}

long sock_shutdown(struct sock *s, int how)
{
    if (!s->tc) return -107;
    if (how == 0 || how == 2) s->shut_rd = 1;
    if (how == 1 || how == 2) { s->shut_wr = 1; tcp_shutdown_wr(s->tc); }
    return 0;
}

void sock_names(struct sock *s, u32 *lip, u16 *lport, u32 *rip, u16 *rport) { *lip = s->lip ? s->lip : net_ip(); *lport = s->lport; *rip = s->rip; *rport = s->rport; }
int sock_is_stream(struct sock *s) { return s->type == SOCK_STREAM_; }
int sock_nread(struct sock *s)
{
    if (s->type == SOCK_DGRAM_) return s->dh != s->dt ? s->dq[s->dt % DGRAMS].len : 0;
    return s->tc ? tcp_readable(s->tc) : 0;
}
