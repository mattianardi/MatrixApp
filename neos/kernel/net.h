#pragma once
#include "kernel.h"

/* IPv4 addresses are kept in host byte order (a.b.c.d == a<<24|b<<16|c<<8|d). */
static inline u16 be16(u16 v) { return __builtin_bswap16(v); }
static inline u32 be32(u32 v) { return __builtin_bswap32(v); }
#define IP4(a,b,c,d) (((u32)(a)<<24)|((u32)(b)<<16)|((u32)(c)<<8)|(u32)(d))

/* pci.c */
struct pci_dev { u8 bus, dev, fn; u16 vendor, device; u8 class, subclass; };
u32  pci_read(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
int  pci_count(void);
int  pci_get(int i, struct pci_dev *out);
int  pci_find(u16 vendor, u16 device, struct pci_dev *out);
void pci_scan(void);

/* nic.c: drivers (virtio-net, e1000/e1000e, RTL8139) behind one interface */
struct nic_driver {
    const char *name;
    int  (*init)(void);                                  /* 0 = device found and running */
    int  (*send)(const u8 *frame, int len);
    void (*poll)(void (*cb)(const u8 *frame, int len));
    const u8 *(*mac)(void);
    void (*stats)(u64 *rx, u64 *tx, u64 *drop);
};
int  nic_probe(void);
const char *nic_name(void);
int  nic_send(const u8 *frame, int len);
void nic_poll(void (*cb)(const u8 *frame, int len));
const u8 *nic_mac(void);
void nic_stats(u64 *rx, u64 *tx, u64 *rxdrop);

/* net.c */
void net_init(void);
void net_start(void);
int  net_up(void);                                   /* has an IP address */
u32  net_ip(void);
int  net_dhcp(void);                                 /* blocking; 0 = configured */
int  net_configure(u32 ip, u32 mask, u32 gw, u32 dns);
int  net_ping(u32 ip, u64 timeout_ms, u64 *rtt_us);
int  net_resolve(const char *name, u32 *ip);         /* dotted quad or DNS A lookup */
int  ip_parse(const char *s, u32 *ip);
void ip_format(u32 ip, char *buf, size_t n);
int  net_nic_present(void);
int  net_wants_dhcp(void);
void net_stats_text(char *buf, size_t n);
typedef void (*udp_cb)(u32 src, u16 sport, const u8 *data, int len);
int  udp_bind(u16 port, udp_cb cb);
int  udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, int len);
int  ip_tx(u32 dst, u8 proto, const u8 *payload, int len);
u16  ip_checksum(const void *data, int len, u32 seed);
u32  ip_pseudo_sum(u32 src, u32 dst, u8 proto, int len);
void tcp_input(u32 src, const u8 *seg, int len);

/* tcp.c */
struct tcp_conn;
struct tcp_conn *tcp_connect(u32 ip, u16 port, u64 timeout_ms);
int  tcp_send(struct tcp_conn *c, const void *data, int len);
int  tcp_recv(struct tcp_conn *c, void *buf, int cap, u64 timeout_ms);   /* >0 bytes, 0 = peer closed, <0 timeout/reset */
void tcp_close(struct tcp_conn *c);
void tcp_tick(void);
struct tcp_listener;
int  tcp_available(struct tcp_conn *c);
int  tcp_is_open(struct tcp_conn *c);
int  tcp_peer(struct tcp_conn *c, u32 *ip, u16 *port, u16 *lport);
void tcp_shutdown_wr(struct tcp_conn *c);
struct tcp_listener *tcp_listen(u16 port);
int  tcp_listener_ready(struct tcp_listener *l);
struct tcp_conn *tcp_accept_try(struct tcp_listener *l);
void tcp_unlisten(struct tcp_listener *l);
u32  tcp_rx_gen(struct tcp_conn *c);
u32  tcp_listener_gen(struct tcp_listener *l);
int  tcp_readable(struct tcp_conn *c);
typedef void (*udp_cb2)(void *ctx, u32 src, u16 sport, const u8 *data, int len);
int  udp_bind_ctx(u16 port, udp_cb2 cb, void *ctx);
void udp_unbind(u16 port);
u16  udp_ephemeral(void);

/* http.c */
int  http_get(const char *url, void *buf, size_t cap, size_t *len, int *status, const char **err);
