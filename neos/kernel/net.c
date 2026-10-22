#include "net.h"

/*
 * Small IPv4 stack: Ethernet, ARP, IPv4 (no fragmentation), ICMP echo, UDP, DHCP, DNS (A records).
 * TCP lives in tcp.c. A polling thread (`net`) feeds received frames in; every knob is a tunable.
 */
struct eth  { u8 dst[6], src[6]; u16 type; } __attribute__((packed));
struct arp  { u16 htype, ptype; u8 hlen, plen; u16 op; u8 sha[6]; u32 spa; u8 tha[6]; u32 tpa; } __attribute__((packed));
struct ip   { u8 vihl, tos; u16 len, id, frag; u8 ttl, proto; u16 csum; u32 src, dst; } __attribute__((packed));
struct udp  { u16 sport, dport, len, csum; } __attribute__((packed));
struct icmp { u8 type, code; u16 csum, id, seq; } __attribute__((packed));

static u32 my_ip, my_mask, my_gw, my_dns;
static int nic_ok;
static struct tunable *t_ip, *t_gw, *t_dns, *t_mask, *t_mac, *t_dhcp, *t_poll, *t_prio, *t_arp_ms, *t_dns_ms;
static const u8 bcast[6] = { 255, 255, 255, 255, 255, 255 };

static struct { u32 ip; u8 mac[6]; int valid; } arpc[8];
static int arp_next;

static struct { u16 port; udp_cb cb; } udps[6];

/* ping state */
static volatile int ping_wait;
static volatile u16 ping_id, ping_seq;
static volatile u64 ping_t0, ping_rtt;
static u64 n_icmp_rx, n_arp_rx, n_ip_rx, n_udp_rx, n_bad;

int ip_parse(const char *s, u32 *ip)
{
    u32 v = 0; int parts = 0;
    for (;;) {
        if (!is_digit(*s)) return -1;
        u32 n = 0;
        while (is_digit(*s)) { n = n * 10 + (*s++ - '0'); if (n > 255) return -1; }
        v = (v << 8) | n; parts++;
        if (parts == 4) break;
        if (*s++ != '.') return -1;
    }
    if (*s) return -1;
    *ip = v;
    return 0;
}

void ip_format(u32 ip, char *b, size_t n)
{
    ksnprintf(b, n, "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

u16 ip_checksum(const void *data, int len, u32 sum)
{
    const u8 *p = data;
    for (; len > 1; len -= 2, p += 2) sum += (u32)(p[0] << 8 | p[1]);
    if (len) sum += (u32)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

u32 ip_pseudo_sum(u32 src, u32 dst, u8 proto, int len)
{
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

static void set_str(struct tunable *t, const char *s) { strlcpy(t->sval, s, sizeof t->sval); }
static void publish(void)
{
    char b[24];
    ip_format(my_ip, b, sizeof b); set_str(t_ip, b);
    ip_format(my_mask, b, sizeof b); set_str(t_mask, b);
    ip_format(my_gw, b, sizeof b); set_str(t_gw, b);
    ip_format(my_dns, b, sizeof b); set_str(t_dns, b);
}

int net_up(void) { return nic_ok && my_ip != 0; }
u32 net_ip(void) { return my_ip; }

int net_configure(u32 ip, u32 mask, u32 gw, u32 dns)
{
    my_ip = ip; my_mask = mask; my_gw = gw; my_dns = dns;
    memset(arpc, 0, sizeof arpc);
    publish();
    return 0;
}

/* ---- transmit ---- */
static int eth_send(const u8 *dst, u16 type, const u8 *payload, int len)
{
    u8 frame[1536];
    struct eth *e = (struct eth *)frame;
    if (len > 1500) return -1;
    memcpy(e->dst, dst, 6); memcpy(e->src, e1000_mac(), 6); e->type = be16(type);
    memcpy(frame + sizeof *e, payload, len);
    if (len + (int)sizeof *e < 60) memset(frame + sizeof *e + len, 0, 60 - sizeof *e - len);
    return e1000_send(frame, (int)sizeof *e + len);
}

static void arp_request(u32 ip)
{
    struct arp a = { be16(1), be16(0x0800), 6, 4, be16(1), {0}, be32(my_ip), {0}, be32(ip) };
    memcpy(a.sha, e1000_mac(), 6);
    eth_send(bcast, 0x0806, (u8 *)&a, sizeof a);
}

static int arp_lookup(u32 ip, u8 *mac)
{
    for (int i = 0; i < 8; i++) if (arpc[i].valid && arpc[i].ip == ip) { memcpy(mac, arpc[i].mac, 6); return 0; }
    return -1;
}

static int resolve_mac(u32 ip, u8 *mac)
{
    u32 hop = ((ip ^ my_ip) & my_mask) == 0 ? ip : my_gw;
    if (arp_lookup(hop, mac) == 0) return 0;
    for (int tries = 0; tries < 3; tries++) {
        arp_request(hop);
        u64 until = ticks + ms_to_ticks((u64)t_arp_ms->val);
        while (ticks < until) { if (arp_lookup(hop, mac) == 0) return 0; thread_sleep_until(ticks + 1); }
    }
    return -1;
}

static u16 ip_id;
static int ip_tx_mac(const u8 *dmac, u32 src, u32 dst, u8 proto, const u8 *payload, int len)
{
    u8 pkt[1500];
    struct ip *h = (struct ip *)pkt;
    if (len > 1500 - (int)sizeof *h) return -1;
    h->vihl = 0x45; h->tos = 0; h->len = be16((u16)(sizeof *h + len)); h->id = be16(ip_id++); h->frag = be16(0x4000);
    h->ttl = 64; h->proto = proto; h->csum = 0; h->src = be32(src); h->dst = be32(dst);
    h->csum = be16(ip_checksum(h, sizeof *h, 0));
    memcpy(pkt + sizeof *h, payload, len);
    return eth_send(dmac, 0x0800, pkt, (int)sizeof *h + len);
}

int ip_tx(u32 dst, u8 proto, const u8 *payload, int len)
{
    u8 mac[6];
    if (!net_up()) return -1;
    if (resolve_mac(dst, mac)) return -1;
    return ip_tx_mac(mac, my_ip, dst, proto, payload, len);
}

int udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, int len)
{
    u8 buf[1480];
    struct udp *u = (struct udp *)buf;
    if (len > 1400) return -1;
    u->sport = be16(sport); u->dport = be16(dport); u->len = be16((u16)(sizeof *u + len)); u->csum = 0;
    memcpy(buf + sizeof *u, data, len);
    return ip_tx(dst, 17, buf, (int)sizeof *u + len);
}

int udp_bind(u16 port, udp_cb cb)
{
    for (int i = 0; i < 6; i++) if (!udps[i].port || udps[i].port == port) { udps[i].port = port; udps[i].cb = cb; return 0; }
    return -1;
}

/* ---- receive ---- */
static void rx_arp(const u8 *p, int len)
{
    if (len < (int)sizeof(struct arp)) return;
    const struct arp *a = (const void *)p;
    n_arp_rx++;
    u32 spa = be32(a->spa), tpa = be32(a->tpa);
    if (spa) {
        int slot = -1;
        for (int i = 0; i < 8; i++) if (arpc[i].valid && arpc[i].ip == spa) slot = i;
        if (slot < 0) { slot = arp_next++ & 7; }
        arpc[slot].ip = spa; memcpy(arpc[slot].mac, a->sha, 6); arpc[slot].valid = 1;
    }
    if (be16(a->op) == 1 && my_ip && tpa == my_ip) {
        struct arp r = { be16(1), be16(0x0800), 6, 4, be16(2), {0}, be32(my_ip), {0}, a->spa };
        memcpy(r.sha, e1000_mac(), 6); memcpy(r.tha, a->sha, 6);
        eth_send(a->sha, 0x0806, (u8 *)&r, sizeof r);
    }
}

static void rx_icmp(u32 src, const u8 *p, int len)
{
    if (len < (int)sizeof(struct icmp)) return;
    const struct icmp *ic = (const void *)p;
    n_icmp_rx++;
    if (ic->type == 8 && ip_checksum(p, len, 0) == 0) {
        u8 buf[1500];
        memcpy(buf, p, len);
        struct icmp *r = (struct icmp *)buf;
        r->type = 0; r->csum = 0; r->csum = be16(ip_checksum(buf, len, 0));
        ip_tx(src, 1, buf, len);
    } else if (ic->type == 0 && ping_wait && be16(ic->id) == ping_id && be16(ic->seq) == ping_seq) {
        ping_rtt = (time_ns() - ping_t0) / 1000;
        ping_wait = 0;
    }
}

static void rx_ip(const u8 *p, int len)
{
    if (len < (int)sizeof(struct ip)) return;
    const struct ip *h = (const void *)p;
    int hl = (h->vihl & 15) * 4, tl = be16(h->len);
    if ((h->vihl >> 4) != 4 || hl < 20 || tl > len || tl < hl) { n_bad++; return; }
    if (be16(h->frag) & 0x3FFF) { n_bad++; return; }                          /* fragments are not reassembled */
    u32 dst = be32(h->dst), src = be32(h->src);
    int mine = !my_ip || dst == my_ip || dst == 0xFFFFFFFF || (my_ip && dst == (my_ip | ~my_mask));
    if (!mine) return;
    n_ip_rx++;
    const u8 *pl = p + hl; int pn = tl - hl;
    switch (h->proto) {
    case 1: rx_icmp(src, pl, pn); break;
    case 6: tcp_input(src, pl, pn); break;
    case 17:
        if (pn >= (int)sizeof(struct udp)) {
            const struct udp *u = (const void *)pl;
            int ul = be16(u->len); if (ul > pn || ul < (int)sizeof *u) return;
            n_udp_rx++;
            for (int i = 0; i < 6; i++) if (udps[i].port == be16(u->dport)) { udps[i].cb(src, be16(u->sport), pl + sizeof *u, ul - (int)sizeof *u); break; }
        }
        break;
    }
}

static void rx_frame(const u8 *f, int len)
{
    if (len < (int)sizeof(struct eth)) return;
    const struct eth *e = (const void *)f;
    u16 t = be16(e->type);
    if (t == 0x0806) rx_arp(f + sizeof *e, len - (int)sizeof *e);
    else if (t == 0x0800) rx_ip(f + sizeof *e, len - (int)sizeof *e);
}

static void net_thread(void *arg)
{
    (void)arg;
    for (;;) {
        e1000_poll(rx_frame);
        tcp_tick();
        thread_sleep_ms((u64)t_poll->val);
    }
}

/* ---- ping ---- */
int net_ping(u32 ip, u64 timeout_ms, u64 *rtt_us)
{
    u8 buf[64]; static u16 seq;
    struct icmp *ic = (struct icmp *)buf;
    memset(buf, 0, sizeof buf);
    ic->type = 8; ic->id = be16(0x4E45); ic->seq = be16(++seq);
    for (int i = 8; i < 64; i++) buf[i] = (u8)i;
    ic->csum = be16(ip_checksum(buf, sizeof buf, 0));
    ping_id = 0x4E45; ping_seq = seq; ping_wait = 1; ping_t0 = time_ns();
    if (ip_tx(ip, 1, buf, sizeof buf)) { ping_wait = 0; return -1; }
    u64 until = ticks + ms_to_ticks(timeout_ms);
    while (ping_wait && ticks < until) thread_sleep_until(ticks + 1);
    if (ping_wait) { ping_wait = 0; return -2; }
    *rtt_us = ping_rtt;
    return 0;
}

/* ---- DHCP ---- */
static volatile int dhcp_stage;                 /* 0 idle, 1 wait offer, 2 wait ack, 3 done */
static u32 dhcp_xid, d_ip, d_mask, d_gw, d_dns, d_srv;

static void dhcp_send(int type)
{
    u8 p[300]; memset(p, 0, sizeof p);
    p[0] = 1; p[1] = 1; p[2] = 6;
    p[4] = (u8)(dhcp_xid >> 24); p[5] = (u8)(dhcp_xid >> 16); p[6] = (u8)(dhcp_xid >> 8); p[7] = (u8)dhcp_xid;
    p[10] = 0x80;                                              /* ask for broadcast replies: we have no address yet */
    memcpy(p + 28, e1000_mac(), 6);
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = (u8)type;
    if (type == 3) {
        p[o++] = 50; p[o++] = 4; p[o++] = (u8)(d_ip >> 24); p[o++] = (u8)(d_ip >> 16); p[o++] = (u8)(d_ip >> 8); p[o++] = (u8)d_ip;
        p[o++] = 54; p[o++] = 4; p[o++] = (u8)(d_srv >> 24); p[o++] = (u8)(d_srv >> 16); p[o++] = (u8)(d_srv >> 8); p[o++] = (u8)d_srv;
    }
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;
    p[o++] = 255;

    u8 buf[400]; struct udp *u = (struct udp *)buf;
    u->sport = be16(68); u->dport = be16(67); u->len = be16((u16)(sizeof *u + o)); u->csum = 0;
    memcpy(buf + sizeof *u, p, o);
    ip_tx_mac(bcast, 0, 0xFFFFFFFF, 17, buf, (int)sizeof *u + o);
}

static void dhcp_rx(u32 src, u16 sport, const u8 *p, int len)
{
    (void)src; (void)sport;
    if (len < 241 || p[0] != 2 || p[236] != 99 || p[239] != 99) return;
    u32 xid = (u32)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
    if (xid != dhcp_xid) return;
    int type = 0; u32 mask = 0, gw = 0, dns = 0, srv = 0;
    for (int i = 240; i + 1 < len && p[i] != 255;) {
        if (p[i] == 0) { i++; continue; }
        int ol = p[i + 1]; if (i + 2 + ol > len) break;
        const u8 *v = p + i + 2;
        u32 w = ol >= 4 ? (u32)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3] : 0;
        if (p[i] == 53) type = v[0]; else if (p[i] == 1) mask = w; else if (p[i] == 3) gw = w; else if (p[i] == 6) dns = w; else if (p[i] == 54) srv = w;
        i += 2 + ol;
    }
    u32 yi = (u32)p[16] << 24 | p[17] << 16 | p[18] << 8 | p[19];
    if (type == 2 && dhcp_stage == 1) { d_ip = yi; d_mask = mask ? mask : 0xFFFFFF00; d_gw = gw; d_dns = dns; d_srv = srv; dhcp_stage = 2; }
    else if (type == 5 && dhcp_stage == 2) { d_ip = yi; if (mask) d_mask = mask; if (gw) d_gw = gw; if (dns) d_dns = dns; dhcp_stage = 3; }
}

int net_dhcp(void)
{
    if (!nic_ok) return -1;
    u32 ip0 = my_ip;
    my_ip = 0; my_mask = 0;
    dhcp_xid = (u32)(time_ns() & 0xFFFFFFFF) ^ 0x4E454F53;
    udp_bind(68, dhcp_rx);
    for (int stage = 1; stage <= 2; stage++) {
        int ok = 0;
        for (int tries = 0; tries < 4 && !ok; tries++) {
            dhcp_stage = stage;
            dhcp_send(stage == 1 ? 1 : 3);
            u64 until = ticks + ms_to_ticks(1500);
            while (ticks < until && dhcp_stage == stage) thread_sleep_until(ticks + 1);
            ok = dhcp_stage != stage;
        }
        if (!ok) { my_ip = ip0; dhcp_stage = 0; return -1; }
    }
    net_configure(d_ip, d_mask, d_gw, d_dns);
    dhcp_stage = 0;
    return 0;
}

/* ---- DNS ---- */
static volatile int dns_wait;
static volatile u32 dns_ans;
static u16 dns_id;
static struct { char name[48]; u32 ip; } dnsc[8];
static int dnsc_next;

static void dns_rx(u32 src, u16 sport, const u8 *p, int len)
{
    (void)src; (void)sport;
    if (len < 12 || ((u16)(p[0] << 8 | p[1])) != dns_id || !dns_wait) return;
    int an = p[6] << 8 | p[7], rc = p[3] & 15;
    if (rc) { dns_ans = 0; dns_wait = 0; return; }
    int i = 12;
    for (int q = p[4] << 8 | p[5]; q > 0; q--) {            /* skip questions */
        while (i < len && p[i]) { if ((p[i] & 0xC0) == 0xC0) { i++; break; } i += p[i] + 1; }
        i += 1 + 4;
    }
    for (; an > 0 && i + 12 <= len; an--) {
        while (i < len && p[i]) { if ((p[i] & 0xC0) == 0xC0) { i++; break; } i += p[i] + 1; }
        i++;
        int type = p[i] << 8 | p[i + 1], rl = p[i + 8] << 8 | p[i + 9];
        i += 10;
        if (type == 1 && rl == 4 && i + 4 <= len) { dns_ans = (u32)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3]; dns_wait = 0; return; }
        i += rl;
    }
    dns_ans = 0; dns_wait = 0;
}

int net_resolve(const char *name, u32 *ip)
{
    if (!ip_parse(name, ip)) return 0;
    for (int i = 0; i < 8; i++) if (dnsc[i].ip && !strcasecmp(dnsc[i].name, name)) { *ip = dnsc[i].ip; return 0; }
    if (!net_up() || !my_dns) return -1;
    u8 q[128]; int o = 12;
    memset(q, 0, 12);
    dns_id = (u16)(time_ns() >> 4); q[0] = (u8)(dns_id >> 8); q[1] = (u8)dns_id; q[2] = 0x01; q[5] = 1;
    for (const char *s = name; *s;) {
        const char *d = s; while (*d && *d != '.') d++;
        int l = (int)(d - s); if (l == 0 || l > 63 || o + l + 6 > (int)sizeof q) return -1;
        q[o++] = (u8)l; memcpy(q + o, s, l); o += l;
        s = *d ? d + 1 : d;
    }
    q[o++] = 0; q[o++] = 0; q[o++] = 1; q[o++] = 0; q[o++] = 1;
    udp_bind(5353, dns_rx);
    for (int tries = 0; tries < 3; tries++) {
        dns_wait = 1; dns_ans = 0;
        if (udp_send(my_dns, 5353, 53, q, o)) continue;
        u64 until = ticks + ms_to_ticks((u64)t_dns_ms->val);
        while (dns_wait && ticks < until) thread_sleep_until(ticks + 1);
        if (!dns_wait) break;
    }
    int ok = !dns_wait && dns_ans;
    dns_wait = 0;
    if (!ok) return -1;
    *ip = dns_ans;
    int s = dnsc_next++ & 7; strlcpy(dnsc[s].name, name, sizeof dnsc[s].name); dnsc[s].ip = dns_ans;
    return 0;
}

void net_stats_text(char *b, size_t n)
{
    u64 r, t, d; e1000_stats(&r, &t, &d);
    ksnprintf(b, n, "rx %lu frames, tx %lu frames, tx-ring-full %lu | arp %lu icmp %lu ip %lu udp %lu bad %lu",
              r, t, d, n_arp_rx, n_icmp_rx, n_ip_rx, n_udp_rx, n_bad);
}

void net_init(void)
{
    module_register("net", "0.1", "e1000 + ARP/IPv4/ICMP/UDP/TCP, DHCP, DNS client");
    t_dhcp   = tune_bool("net", "dhcp",    "configure the interface with DHCP at boot", TF_BOOT, 1, NULL);
    t_poll   = tune_int ("net", "poll_ms", "receive polling interval (ms); lower = lower latency, more CPU", TF_LIVE, 1, 1, 100, NULL);
    t_prio   = tune_int ("net", "prio",    "priority of the network thread", TF_BOOT, 2, 0, 6, NULL);
    t_arp_ms = tune_int ("net", "arp_timeout_ms", "wait per ARP attempt", TF_LIVE, 300, 20, 5000, NULL);
    t_dns_ms = tune_int ("net", "dns_timeout_ms", "wait per DNS attempt", TF_LIVE, 1500, 100, 10000, NULL);
    t_ip     = tune_str ("net", "ip",      "current IPv4 address", TF_RO, "0.0.0.0", NULL);
    t_mask   = tune_str ("net", "mask",    "netmask", TF_RO, "0.0.0.0", NULL);
    t_gw     = tune_str ("net", "gateway", "default gateway", TF_RO, "0.0.0.0", NULL);
    t_dns    = tune_str ("net", "dns",     "DNS server", TF_RO, "0.0.0.0", NULL);
    t_mac    = tune_str ("net", "mac",     "interface MAC", TF_RO, "-", NULL);
    pci_scan();
    if (e1000_init() == 0) {
        nic_ok = 1;
        const u8 *m = e1000_mac(); char b[24];
        ksnprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        set_str(t_mac, b);
    }
}

void net_start(void)
{
    if (!nic_ok) return;
    thread_create("net", (u8)t_prio->val, net_thread, NULL);
}

int net_nic_present(void) { return nic_ok; }
int net_wants_dhcp(void) { return (int)t_dhcp->val; }
