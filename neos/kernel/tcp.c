#include "net.h"

/*
 * TCP client: active open, in-order receive into a ring (out-of-order data is dropped and
 * re-ACKed so the peer retransmits), stop-and-wait send with retransmission. Enough for HTTP.
 */
#define MAXC  4
#define RXSZ  32768
enum { S_FREE, S_SYN, S_EST, S_CLOSED };
#define F_FIN 1
#define F_SYN 2
#define F_RST 4
#define F_PSH 8
#define F_ACK 16

struct tcp_conn {
    int state;
    u32 rip; u16 rport, lport;
    u32 snd_una, snd_nxt, rcv_nxt;
    volatile u32 whead, rtail;
    volatile int fin, rst;
    u32 adv_win;
    u8 *rx;
};
struct tcph { u16 sport, dport; u32 seq, ack; u16 off_flags, win, csum, urg; } __attribute__((packed));

static struct tcp_conn conns[MAXC];
static u16 next_port = 49152;
static struct tunable *t_mss, *t_rto, *t_retries;

static void tcp_tunables(void)
{
    if (t_mss) return;
    t_mss     = tune_int("net", "tcp_mss",     "TCP segment payload size", TF_LIVE, 1400, 256, 1460, NULL);
    t_rto     = tune_int("net", "tcp_rto_ms",  "TCP retransmission timeout", TF_LIVE, 400, 20, 5000, NULL);
    t_retries = tune_int("net", "tcp_retries", "TCP retransmissions before giving up", TF_LIVE, 6, 1, 20, NULL);
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
    return ip_tx(rip, 6, buf, hl + len);
}

static int send_ctl(struct tcp_conn *c, u8 flags)
{
    return send_seg(c, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, flags, free_space(c), NULL, 0, 0);
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
    c->whead = c->rtail = 0; c->fin = c->rst = 0;
    u32 iss = (u32)time_ns() * 2654435761u;
    c->snd_una = iss; c->snd_nxt = iss + 1; c->rcv_nxt = 0;
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

int tcp_send(struct tcp_conn *c, const void *data, int len)
{
    const u8 *p = data;
    while (len > 0) {
        int n = len < t_mss->val ? len : (int)t_mss->val;
        u32 seq = c->snd_nxt, end = seq + n;
        c->snd_nxt = end;
        int ok = 0;
        for (int tries = 0; tries < t_retries->val && !ok && !c->rst; tries++) {
            send_seg(c, c->rip, c->lport, c->rport, seq, c->rcv_nxt, F_ACK | F_PSH, free_space(c), p, n, 0);
            u64 until = ticks + ms_to_ticks((u64)t_rto->val);
            while (ticks < until && (int)(end - c->snd_una) > 0 && !c->rst) thread_sleep_until(ticks + 1);
            ok = (int)(end - c->snd_una) <= 0;
        }
        if (!ok) return -1;
        p += n; len -= n;
    }
    return 0;
}

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

void tcp_close(struct tcp_conn *c)
{
    if (c->state == S_EST && !c->rst) {
        u32 fin_seq = c->snd_nxt;
        c->snd_nxt++;
        for (int tries = 0; tries < 3; tries++) {
            send_seg(c, c->rip, c->lport, c->rport, fin_seq, c->rcv_nxt, F_FIN | F_ACK, free_space(c), NULL, 0, 0);
            u64 until = ticks + ms_to_ticks(300);
            while (ticks < until && (int)(c->snd_nxt - c->snd_una) > 0) thread_sleep_until(ticks + 1);
            if ((int)(c->snd_nxt - c->snd_una) <= 0) break;
        }
    }
    c->state = S_FREE;
}

void tcp_tick(void) { }

void tcp_input(u32 src, const u8 *seg, int len)
{
    if (len < 20) return;
    const struct tcph *h = (const void *)seg;
    int hl = (be16(h->off_flags) >> 12) * 4, flags = be16(h->off_flags) & 0x3F;
    if (hl < 20 || hl > len) return;
    if (ip_checksum(seg, len, ip_pseudo_sum(src, net_ip(), 6, len)) != 0) return;
    u16 sp = be16(h->sport), dp = be16(h->dport);
    u32 seq = be32(h->seq), ack = be32(h->ack);
    int plen = len - hl;

    struct tcp_conn *c = NULL;
    for (int i = 0; i < MAXC; i++)
        if (conns[i].state != S_FREE && conns[i].rip == src && conns[i].rport == sp && conns[i].lport == dp) c = &conns[i];
    if (!c) {
        if (!(flags & F_RST))                                  /* nobody home: tell the peer to stop */
            send_seg(NULL, src, dp, sp, (flags & F_ACK) ? ack : 0, seq + plen + ((flags & (F_SYN | F_FIN)) ? 1 : 0),
                     (flags & F_ACK) ? F_RST : (F_RST | F_ACK), 0, NULL, 0, 0);
        return;
    }
    if (flags & F_RST) { c->rst = 1; c->state = S_CLOSED; return; }

    if (c->state == S_SYN) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1; c->snd_una = ack; c->state = S_EST;
            send_ctl(c, F_ACK);
        }
        return;
    }
    if (flags & F_ACK) {
        if ((int)(ack - c->snd_una) > 0 && (int)(c->snd_nxt - ack) >= 0) c->snd_una = ack;
    }
    int acked_now = 0;
    if (plen > 0) {
        if (seq == c->rcv_nxt) {
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
