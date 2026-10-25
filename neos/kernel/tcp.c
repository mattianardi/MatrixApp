#include "net.h"

/*
 * TCP: active and passive open, in-order receive into a ring (out-of-order data is dropped and re-ACKed so the
 * peer retransmits), and an asynchronous sender. Writers append to a per-connection transmit ring and return;
 * the network thread (tcp_tick) and the writer itself push segments out inside the peer's window, advance the
 * ring when ACKs arrive and retransmit (go-back-N, exponential back-off) when they do not. close() is lazy: the
 * connection lingers until its data and FIN are acknowledged, so a server closing a socket never blocks.
 */
#define MAXC  128
#define MAXL  16
#define LQ    32                                           /* accept queue length */
#define RXSZ  32768
#define TXSZ  65536
enum { S_FREE, S_SYN, S_EST, S_CLOSED, S_RCVD };
#define F_FIN 1
#define F_SYN 2
#define F_RST 4
#define F_PSH 8
#define F_ACK 16

struct tcp_conn {
    int state;
    u32 rip; u16 rport, lport;
    u32 seq0;                                              /* sequence number of byte 0 of the send stream */
    u32 snd_una, snd_nxt, rcv_nxt;                         /* snd_una = seq0 + ttail, snd_nxt = seq0 + tsent (+1 once the FIN is out) */
    volatile u32 whead, rtail;                             /* receive ring */
    volatile int fin, rst, fin_sent;
    struct tcp_listener *listener;
    u32 adv_win;
    u8 *rx;
    u8 *tx; volatile u32 thead, ttail, tsent;              /* transmit ring: bytes appended / acknowledged / sent at least once */
    u32 peer_win;
    u64 rto_at, born, linger_until, probe_at;
    int tries, dupacks;
    volatile int busy;
    u8 fin_pending, linger;
};
struct tcp_listener { int used; u16 port; struct tcp_conn *q[LQ]; volatile u32 qh, qt; };
struct tcph { u16 sport, dport; u32 seq, ack; u16 off_flags, win, csum, urg; } __attribute__((packed));

static struct tcp_conn conns[MAXC];
static struct tcp_listener listeners[MAXL];
extern int tcp_rx_loopback;
static u16 next_port = 49152;
static struct tunable *t_mss, *t_rto, *t_retries, *t_cwnd, *t_trace;

static void tcp_tunables(void)
{
    if (t_mss) return;
    t_mss     = tune_int("net", "tcp_mss",     "TCP segment payload size", TF_LIVE, 1400, 256, 1460, NULL);
    t_rto     = tune_int("net", "tcp_rto_ms",  "TCP retransmission timeout", TF_LIVE, 400, 20, 5000, NULL);
    t_retries = tune_int("net", "tcp_retries", "TCP retransmissions before giving up", TF_LIVE, 6, 1, 20, NULL);
    t_trace   = tune_bool("net", "tcp_trace", "print every TCP segment (debugging)", TF_LIVE, 0, NULL);
    t_cwnd    = tune_int("net", "tcp_cwnd_segs", "TCP segments in flight per connection", TF_LIVE, 8, 1, 44, NULL);
}

static u32 free_space(struct tcp_conn *c) { return RXSZ - (c->whead - c->rtail); }

static int send_seg(struct tcp_conn *c, u32 rip, u16 lport, u16 rport, u32 seq, u32 ack, u8 flags, u32 win,
                    const void *data, int len, int mss_opt)
{
    u8 buf[1500];
    struct tcph *h = (struct tcph *)buf;
    int hl = mss_opt ? 24 : 20;
    if (len > 1460) return -1;
    h->sport = be16(lport); h->dport = be16(rport); h->seq = be32(seq); h->ack = be32(ack);
    h->off_flags = be16((u16)((hl / 4) << 12 | flags)); h->win = be16(win > 65535 ? 65535 : (u16)win); h->csum = 0; h->urg = 0;
    if (mss_opt) { buf[20] = 2; buf[21] = 4; buf[22] = (u8)(mss_opt >> 8); buf[23] = (u8)mss_opt; }
    if (len) memcpy(buf + hl, data, len);
    h->csum = be16(ip_checksum(buf, hl + len, ip_pseudo_sum(net_ip(), rip, 6, hl + len)));
    if (c) c->adv_win = win;
    if (t_trace && t_trace->val) kprintf("[tcp] out %u>%u %s%s%s%s seq %u ack %u len %d win %u\n", lport, rport, flags & F_SYN ? "S" : "", flags & F_FIN ? "F" : "", flags & F_RST ? "R" : "", flags & F_ACK ? "A" : "", seq - (c ? c->seq0 : 0), c ? ack - c->rcv_nxt + c->rcv_nxt : ack, len, win);
    return ip_tx(rip, 6, buf, hl + len);
}

static int send_ctl(struct tcp_conn *c, u8 flags)
{
    return send_seg(c, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, flags, free_space(c), NULL, 0, 0);
}

static void conn_reset_tx(struct tcp_conn *c, u32 iss)
{
    c->seq0 = iss + 1; c->snd_una = iss; c->snd_nxt = iss + 1;
    c->thead = c->ttail = c->tsent = 0; c->peer_win = 65535; c->tries = c->dupacks = 0; c->fin_pending = c->linger = 0; c->busy = 0;
    c->rto_at = c->probe_at = c->linger_until = 0; c->born = ticks;
}

struct tcp_conn *tcp_connect(u32 ip, u16 port, u64 timeout_ms)
{
    tcp_tunables();
    struct tcp_conn *c = NULL;
    u64 fl = irq_save();
    for (int i = 0; i < MAXC; i++) if (conns[i].state == S_FREE) { c = &conns[i]; c->state = S_CLOSED; break; }
    irq_restore(fl);
    if (!c) return NULL;
    if (!c->rx) c->rx = kmalloc(RXSZ);
    if (!c->rx) { c->state = S_FREE; return NULL; }
    c->rip = ip; c->rport = port; c->lport = next_port++; if (next_port < 49152) next_port = 49152;
    c->whead = c->rtail = 0; c->fin = c->rst = c->fin_sent = 0; c->listener = NULL;
    u32 iss = (u32)time_ns() * 2654435761u;
    conn_reset_tx(c, iss);
    c->rcv_nxt = 0;
    c->state = S_SYN;
    u64 deadline = ticks + ms_to_ticks(timeout_ms);
    while (ticks < deadline && c->state == S_SYN && !c->rst) {
        send_seg(c, ip, c->lport, port, iss, 0, F_SYN, free_space(c), NULL, 0, (int)t_mss->val);
        u64 retry = ticks + ms_to_ticks(1000);
        while (ticks < retry && ticks < deadline && c->state == S_SYN && !c->rst) thread_sleep_until(ticks + 1);
    }
    if (c->state != S_EST) { c->state = S_FREE; return NULL; }
    return c;
}

/* ---- sending ---- */
static void tx_pump(struct tcp_conn *c)
{
    if (c->state != S_EST || c->rst) return;
    u64 fl = irq_save();
    if (c->busy) { irq_restore(fl); return; }
    c->busy = 1; irq_restore(fl);
    u32 mss = (u32)t_mss->val, cw = (u32)t_cwnd->val * mss, win = c->peer_win;
    u8 seg[1460];
    while (c->tsent != c->thead) {
        u32 inflight = c->tsent - c->ttail, room = cw < win ? cw : win, n = c->thead - c->tsent;
        if (!room && !inflight && ticks >= c->probe_at) { room = 1; c->probe_at = ticks + ms_to_ticks((u64)t_rto->val * 2); }   /* zero window: probe with one byte */
        if (inflight >= room) break;
        if (n > mss) n = mss;
        if (n > room - inflight) n = room - inflight;
        for (u32 i = 0; i < n; i++) seg[i] = c->tx[(c->tsent + i) % TXSZ];
        send_seg(c, c->rip, c->lport, c->rport, c->seq0 + c->tsent, c->rcv_nxt, F_ACK | F_PSH, free_space(c), seg, (int)n, 0);
        if (!inflight) c->rto_at = ticks + ms_to_ticks((u64)t_rto->val);
        c->tsent += n; c->snd_nxt = c->seq0 + c->tsent;
    }
    if (c->fin_pending && !c->fin_sent && c->tsent == c->thead) {
        send_seg(c, c->rip, c->lport, c->rport, c->seq0 + c->tsent, c->rcv_nxt, F_FIN | F_ACK, free_space(c), NULL, 0, 0);
        c->fin_sent = 1; c->snd_nxt = c->seq0 + c->tsent + 1;
        if (c->snd_una == c->seq0 + c->tsent) c->rto_at = ticks + ms_to_ticks((u64)t_rto->val);
    }
    c->busy = 0;
}

/* returns the bytes accepted: all of them when blocking (or < len if the connection died), possibly 0 when not blocking and the ring is full; -1 = broken */
int tcp_write(struct tcp_conn *c, const void *data, int len, int nonblock)
{
    tcp_tunables();
    if (!c->tx) { c->tx = kmalloc(TXSZ); if (!c->tx) return -1; }
    const u8 *p = data; int done = 0;
    while (done < len) {
        if (c->rst || c->state != S_EST || c->fin_pending) return done ? done : -1;
        u32 space = TXSZ - (c->thead - c->ttail);
        if (!space) { if (nonblock) return done; tx_pump(c); thread_sleep_until(ticks + 1); continue; }
        u32 n = (u32)(len - done) < space ? (u32)(len - done) : space;
        for (u32 i = 0; i < n; i++) c->tx[(c->thead + i) % TXSZ] = p[done + (int)i];
        c->thead += n; done += (int)n;
        tx_pump(c);
    }
    return done;
}
int tcp_send(struct tcp_conn *c, const void *data, int len) { return tcp_write(c, data, len, 0) == len ? 0 : -1; }
int tcp_can_write(struct tcp_conn *c) { return c->state == S_EST && !c->rst && !c->fin_pending && TXSZ - (c->thead - c->ttail) >= 1; }

int tcp_recv(struct tcp_conn *c, void *buf, int cap, u64 timeout_ms)
{
    u64 until = ticks + ms_to_ticks(timeout_ms);
    while (c->whead == c->rtail && !c->fin && !c->rst && ticks < until) thread_sleep_until(ticks + 1);
    u32 avail = c->whead - c->rtail;
    if (!avail) return c->rst ? -2 : c->fin ? 0 : -1;
    int n = avail < (u32)cap ? (int)avail : cap;
    u8 *o = buf;
    for (int i = 0; i < n; i++) o[i] = c->rx[(c->rtail + i) % RXSZ];
    c->rtail += n;
    if (free_space(c) >= c->adv_win + 2 * (u32)t_mss->val || c->adv_win < (u32)t_mss->val) send_ctl(c, F_ACK);   /* window update */
    return n;
}

/* close never blocks: the connection lingers until its data and FIN are acknowledged (the network thread finishes the job) */
void tcp_close(struct tcp_conn *c)
{
    if (c->state == S_EST && !c->rst) {
        c->fin_pending = 1; c->linger = 1;
        if (c->whead != c->rtail) c->rtail = c->whead;              /* nobody will read it */
        tx_pump(c);
        return;
    }
    c->state = S_FREE;
}

void tcp_shutdown_wr(struct tcp_conn *c)
{
    if (c->state != S_EST || c->rst || c->fin_pending) return;
    c->fin_pending = 1;
    tx_pump(c);
}

void tcp_tick(void)
{
    if (!t_mss) return;
    for (int i = 0; i < MAXC; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->state == S_FREE) continue;
        if (c->state == S_RCVD && ticks - c->born > ms_to_ticks(8000)) { c->state = S_FREE; continue; }      /* the handshake never completed */
        if (c->state != S_EST) { if (c->linger && c->state == S_CLOSED) c->state = S_FREE; continue; }
        if (c->rst) { if (c->linger) c->state = S_FREE; continue; }
        tx_pump(c);
        int outstanding = c->tsent != c->ttail || (c->fin_sent && c->snd_una != c->snd_nxt);
        if (outstanding && ticks >= c->rto_at) {
            if (++c->tries > (int)t_retries->val) { c->rst = 1; if (c->linger) c->state = S_FREE; continue; }
            c->tsent = c->ttail; c->fin_sent = 0; c->snd_nxt = c->seq0 + c->tsent;                    /* go-back-N */
            c->rto_at = ticks + ms_to_ticks((u64)t_rto->val << (c->tries < 4 ? c->tries : 4));
            tx_pump(c);
        }
        if (c->linger && c->fin_sent && c->snd_una == c->snd_nxt) {                                   /* our FIN is acknowledged: wait a moment for the peer's */
            if (!c->linger_until) c->linger_until = ticks + ms_to_ticks(1500);
            if (c->fin || ticks >= c->linger_until) c->state = S_FREE;
        }
    }
}

void tcp_input(u32 src, const u8 *seg, int len)
{
    if (len < 20) return;
    tcp_tunables();
    const struct tcph *h = (const void *)seg;
    int hl = (be16(h->off_flags) >> 12) * 4, flags = be16(h->off_flags) & 0x3F;
    if (hl < 20 || hl > len) return;
    if (!tcp_rx_loopback && ip_checksum(seg, len, ip_pseudo_sum(src, net_ip(), 6, len)) != 0) return;
    u16 sp = be16(h->sport), dp = be16(h->dport);
    u32 seq = be32(h->seq), ack = be32(h->ack);
    int plen = len - hl;
    if (t_trace && t_trace->val) kprintf("[tcp] in  %u>%u %s%s%s%s seq %u ack %u len %d win %u\n", sp, dp, flags & F_SYN ? "S" : "", flags & F_FIN ? "F" : "", flags & F_RST ? "R" : "", flags & F_ACK ? "A" : "", seq, ack, plen, be16(h->win));

    struct tcp_conn *c = NULL;
    for (int i = 0; i < MAXC; i++)
        if (conns[i].state != S_FREE && conns[i].rip == src && conns[i].rport == sp && conns[i].lport == dp) c = &conns[i];
    if (!c && (flags & F_SYN) && !(flags & F_ACK)) {                      /* passive open */
        struct tcp_listener *l = NULL;
        for (int i = 0; i < MAXL; i++) if (listeners[i].used && listeners[i].port == dp) l = &listeners[i];
        if (l && l->qh - l->qt < LQ) {
            u64 fl = irq_save();
            for (int i = 0; i < MAXC; i++) if (conns[i].state == S_FREE) { c = &conns[i]; c->state = S_RCVD; break; }
            irq_restore(fl);
            if (c) {
                if (!c->rx) c->rx = kmalloc(RXSZ);
                if (!c->rx) { c->state = S_FREE; return; }
                c->rip = src; c->rport = sp; c->lport = dp; c->whead = c->rtail = 0; c->fin = c->rst = c->fin_sent = 0; c->listener = l;
                u32 iss = (u32)time_ns() * 2654435761u;
                conn_reset_tx(c, iss);
                c->peer_win = be16(h->win);
                c->rcv_nxt = seq + 1;
                send_seg(c, src, dp, sp, iss, c->rcv_nxt, F_SYN | F_ACK, free_space(c), NULL, 0, (int)t_mss->val);
                return;
            }
        }
    }
    if (!c) {
        if (!(flags & F_RST))                                  /* nobody home: tell the peer to stop */
            send_seg(NULL, src, dp, sp, (flags & F_ACK) ? ack : 0, seq + plen + ((flags & (F_SYN | F_FIN)) ? 1 : 0),
                     (flags & F_ACK) ? F_RST : (F_RST | F_ACK), 0, NULL, 0, 0);
        return;
    }
    if (flags & F_RST) { c->rst = 1; c->state = c->linger ? S_FREE : S_CLOSED; return; }

    if (c->state == S_RCVD) {
        if (flags & F_SYN) { send_seg(c, c->rip, c->lport, c->rport, c->snd_una, c->rcv_nxt, F_SYN | F_ACK, free_space(c), NULL, 0, (int)t_mss->val); return; }
        if ((flags & F_ACK) && ack == c->snd_nxt) {
            c->snd_una = ack; c->state = S_EST; c->peer_win = be16(h->win);
            if (c->listener) { c->listener->q[c->listener->qh % LQ] = c; c->listener->qh++; }
        } else return;
    }
    if (c->state == S_SYN) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1; c->snd_una = ack; c->state = S_EST; c->peer_win = be16(h->win);
            send_ctl(c, F_ACK);
        }
        return;
    }
    if (flags & F_ACK) {
        c->peer_win = be16(h->win);
        if ((int)(ack - c->snd_una) > 0 && (int)(c->snd_nxt - ack) >= 0) {
            u32 acked = ack - c->snd_una;
            if (c->fin_sent && ack == c->snd_nxt) acked--;                                         /* the FIN occupies the last sequence number */
            c->ttail += acked; c->snd_una = ack; c->tries = 0; c->dupacks = 0;
            c->rto_at = ticks + ms_to_ticks((u64)t_rto->val);
        } else if (ack == c->snd_una && plen == 0 && !(flags & (F_FIN | F_SYN)) && c->tsent != c->ttail && ++c->dupacks == 3) {
            c->tsent = c->ttail; c->snd_nxt = c->seq0 + c->tsent; c->dupacks = 0;                  /* fast retransmit */
            c->rto_at = ticks + ms_to_ticks((u64)t_rto->val);
        }
        tx_pump(c);
    }
    int acked_now = 0;
    if (plen > 0) {
        if (c->linger) { if (seq == c->rcv_nxt) c->rcv_nxt += plen; }                              /* closed locally: take it and drop it */
        else if (seq == c->rcv_nxt) {
            u32 take = free_space(c) < (u32)plen ? free_space(c) : (u32)plen;
            for (u32 i = 0; i < take; i++) c->rx[(c->whead + i) % RXSZ] = seg[hl + i];
            c->whead += take;
            c->rcv_nxt += take;
            if (take < (u32)plen) { send_ctl(c, F_ACK); return; }
        }
        acked_now = 1;
    }
    if ((flags & F_FIN) && seq + plen == c->rcv_nxt && !c->fin) { c->rcv_nxt++; c->fin = 1; acked_now = 1; }
    if (acked_now) send_ctl(c, F_ACK);
}

int tcp_available(struct tcp_conn *c) { return c->whead != c->rtail || c->fin || c->rst; }
int tcp_is_open(struct tcp_conn *c) { return c->state == S_EST && !c->rst; }
int tcp_peer(struct tcp_conn *c, u32 *ip, u16 *port, u16 *lport) { *ip = c->rip; *port = c->rport; *lport = c->lport; return 0; }

struct tcp_listener *tcp_listen(u16 port)
{
    tcp_tunables();
    for (int i = 0; i < MAXL; i++) if (listeners[i].used && listeners[i].port == port) return NULL;      /* EADDRINUSE */
    for (int i = 0; i < MAXL; i++) if (!listeners[i].used) { memset(&listeners[i], 0, sizeof listeners[i]); listeners[i].port = port; listeners[i].used = 1; return &listeners[i]; }
    return NULL;
}
int tcp_listener_ready(struct tcp_listener *l) { return l->qh != l->qt; }
struct tcp_conn *tcp_accept_try(struct tcp_listener *l)
{
    if (l->qh == l->qt) return NULL;
    struct tcp_conn *c = l->q[l->qt % LQ]; l->qt++;
    return c;
}
void tcp_unlisten(struct tcp_listener *l)
{
    while (l->qh != l->qt) { struct tcp_conn *c = l->q[l->qt % LQ]; l->qt++; tcp_close(c); }
    l->used = 0;
}
u32 tcp_rx_gen(struct tcp_conn *c) { return c->whead + (c->fin ? 1u : 0u) + (c->rst ? 2u : 0u) + (c->state == S_EST ? 4u : 0u); }
u32 tcp_listener_gen(struct tcp_listener *l) { return l->qh; }
int tcp_readable(struct tcp_conn *c) { return (int)(c->whead - c->rtail); }

/* `tcp`: the connection table (state, peer, queued bytes) - what to look at when a server stops answering */
int tcp_dump(int i, int *state, u32 *rip, u16 *rport, u16 *lport, u32 *queued, int *fin, int *rst)
{
    if (i < 0 || i >= MAXC) return -1;
    struct tcp_conn *c = &conns[i];
    *state = c->state; *rip = c->rip; *rport = c->rport; *lport = c->lport; *queued = c->whead - c->rtail; *fin = c->fin; *rst = c->rst;
    return 0;
}
int tcp_listeners_dump(int i, u16 *port, u32 *queued)
{
    if (i < 0 || i >= MAXL || !listeners[i].used) return -1;
    *port = listeners[i].port; *queued = listeners[i].qh - listeners[i].qt; return 0;
}
