#include "net.h"

/* Intel 82540EM-family (QEMU "e1000", VirtualBox, many servers' legacy mode). Polled, DMA via identity-mapped RAM. */
#define RXN 32
#define TXN 16
#define BUFSZ 2048
enum { CTRL = 0x0000, STATUS = 0x0008, EERD = 0x0014, IMC = 0x00D8, RCTL = 0x0100, TCTL = 0x0400, TIPG = 0x0410,
       RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818,
       TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818, MTA = 0x5200, RAL0 = 0x5400, RAH0 = 0x5404 };

struct rxd { u64 addr; u16 len, csum; u8 status, err; u16 special; } __attribute__((packed));
struct txd { u64 addr; u16 len; u8 cso, cmd, status, css; u16 special; } __attribute__((packed));

static volatile u8 *mmio;
static struct rxd *rx;
static struct txd *tx;
static u8 *rxbuf, *txbuf;
static u32 rx_cur, tx_cur;
static u8 mac[6];
static u64 n_rx, n_tx, n_drop;

static inline u32 rd(u32 r) { return *(volatile u32 *)(mmio + r); }
static inline void wr(u32 r, u32 v) { *(volatile u32 *)(mmio + r) = v; }

static u16 eeprom(int word)
{
    wr(EERD, ((u32)word << 8) | 1);
    for (int i = 0; i < 100000; i++) { u32 v = rd(EERD); if (v & 0x10) return (u16)(v >> 16); }
    return 0;
}

int e1000_init(void)
{
    struct pci_dev d;
    if (pci_find(0x8086, 0x100E, &d) && pci_find(0x8086, 0x100F, &d) && pci_find(0x8086, 0x10D3, &d)) return -1;
    u32 cmd = pci_read(d.bus, d.dev, d.fn, 4);
    pci_write(d.bus, d.dev, d.fn, 4, cmd | 0x7);                 /* I/O, memory, bus master */
    u32 bar = pci_read(d.bus, d.dev, d.fn, 0x10) & ~0xFu;
    if (!bar) return -1;
    mmio = (volatile u8 *)(u64)bar;

    wr(IMC, 0xFFFFFFFF);
    wr(CTRL, rd(CTRL) | (1u << 26));                             /* reset */
    for (volatile int i = 0; i < 1000000; i++) { if (!(rd(CTRL) & (1u << 26))) break; }
    wr(IMC, 0xFFFFFFFF);
    wr(CTRL, rd(CTRL) | (1u << 6));                              /* set link up */

    u32 lo = rd(RAL0), hi = rd(RAH0);
    if (!lo && !(hi & 0xFFFF)) { u16 w0 = eeprom(0), w1 = eeprom(1), w2 = eeprom(2); lo = w0 | (w1 << 16); hi = w2; }
    for (int i = 0; i < 4; i++) mac[i] = (u8)(lo >> (8 * i));
    mac[4] = (u8)hi; mac[5] = (u8)(hi >> 8);
    wr(RAL0, lo); wr(RAH0, (hi & 0xFFFF) | (1u << 31));
    for (int i = 0; i < 128; i++) wr(MTA + i * 4, 0);

    u64 rp = pmm_alloc(1), tp = pmm_alloc(1), rb = pmm_alloc(RXN * BUFSZ / 4096), tb = pmm_alloc(TXN * BUFSZ / 4096);
    if (!rp || !tp || !rb || !tb) return -1;
    rx = (struct rxd *)rp; tx = (struct txd *)tp; rxbuf = (u8 *)rb; txbuf = (u8 *)tb;
    memset(rx, 0, 4096); memset(tx, 0, 4096);
    for (int i = 0; i < RXN; i++) rx[i].addr = (u64)(rxbuf + i * BUFSZ);
    for (int i = 0; i < TXN; i++) { tx[i].addr = (u64)(txbuf + i * BUFSZ); tx[i].status = 1; }

    wr(RDBAL, (u32)rp); wr(RDBAH, (u32)(rp >> 32)); wr(RDLEN, RXN * 16); wr(RDH, 0); wr(RDT, RXN - 1);
    wr(RCTL, (1u << 1) | (1u << 15) | (1u << 26));               /* enable, accept broadcast, strip CRC, 2 KiB buffers */
    wr(TDBAL, (u32)tp); wr(TDBAH, (u32)(tp >> 32)); wr(TDLEN, TXN * 16); wr(TDH, 0); wr(TDT, 0);
    wr(TIPG, 0x0060200A);
    wr(TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12));
    return 0;
}

const u8 *e1000_mac(void) { return mac; }

int e1000_send(const u8 *frame, int len)
{
    if (!mmio || len > BUFSZ - 4) return -1;
    if (len < 60) len = 60;                                      /* minimum Ethernet frame (padding is whatever is in the buffer) */
    u64 fl = irq_save();
    struct txd *d = &tx[tx_cur];
    if (!(d->status & 1)) { n_drop++; irq_restore(fl); return -1; }
    u8 *b = txbuf + tx_cur * BUFSZ;
    memcpy(b, frame, len);
    d->len = (u16)len; d->cmd = 0x0B; d->status = 0;             /* EOP | IFCS | RS */
    tx_cur = (tx_cur + 1) % TXN;
    __asm__ volatile("" ::: "memory");
    wr(TDT, tx_cur);
    n_tx++;
    irq_restore(fl);
    return 0;
}

void e1000_poll(void (*cb)(const u8 *, int))
{
    if (!mmio) return;
    while (rx[rx_cur].status & 1) {
        int len = rx[rx_cur].len;
        u8 *b = rxbuf + rx_cur * BUFSZ;
        n_rx++;
        if (!rx[rx_cur].err) cb(b, len);
        rx[rx_cur].status = 0;
        wr(RDT, rx_cur);
        rx_cur = (rx_cur + 1) % RXN;
    }
}

void e1000_stats(u64 *r, u64 *t, u64 *d) { *r = n_rx; *t = n_tx; *d = n_drop; }
