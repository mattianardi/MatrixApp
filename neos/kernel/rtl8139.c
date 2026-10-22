#include "net.h"

/* Realtek RTL8139 (QEMU "rtl8139", old PCs, many hypervisors). I/O-port programmed, one 8 KiB receive ring, four transmit slots. */
#define RXLEN 8192
static u16 io;
static u8 *rxring; static u64 rxphys;
static u8 *txbuf; static u64 txphys;
static u32 rx_off, tx_cur;
static u8 mac[6];
static u64 n_rx, n_tx, n_drop;

static int rtl_init(void)
{
    struct pci_dev d;
    if (pci_find(0x10EC, 0x8139, &d)) return -1;
    pci_write(d.bus, d.dev, d.fn, 4, pci_read(d.bus, d.dev, d.fn, 4) | 0x5);
    u32 bar = pci_read(d.bus, d.dev, d.fn, 0x10);
    if (!(bar & 1)) return -1;
    io = (u16)(bar & ~3u);
    outb((u16)(io + 0x52), 0);                                         /* power on */
    outb((u16)(io + 0x37), 0x10);                                      /* software reset */
    for (int i = 0; i < 100000 && (inb((u16)(io + 0x37)) & 0x10); i++) { }
    for (int i = 0; i < 6; i++) mac[i] = inb((u16)(io + i));
    rxphys = pmm_alloc((RXLEN + 16 + 1500 + 4095) / 4096); txphys = pmm_alloc(2);
    if (!rxphys || !txphys) return -1;
    rxring = P2V(rxphys); txbuf = P2V(txphys);
    memset(rxring, 0, RXLEN + 16 + 1500);
    outl((u16)(io + 0x30), (u32)rxphys);
    outb((u16)(io + 0x37), 0x0C);                                      /* receiver + transmitter on */
    outl((u16)(io + 0x44), 0x8F);                                      /* accept all/broadcast/multicast/physical, WRAP */
    outl((u16)(io + 0x40), 0x03000700);                                /* TCR: standard IFG, max DMA burst */
    outw((u16)(io + 0x3C), 0);                                         /* no interrupts: polled */
    return 0;
}

static const u8 *rtl_mac(void) { return mac; }

static int rtl_send(const u8 *frame, int len)
{
    if (len > 1792) return -1;
    u64 fl = irq_save();
    u16 tsd = (u16)(io + 0x10 + 4 * tx_cur);
    if (!(inl(tsd) & (1u << 13)) && n_tx > 3) { n_drop++; irq_restore(fl); return -1; }      /* slot still owned by the NIC */
    u8 *b = txbuf + tx_cur * 2048;
    memcpy(b, frame, (size_t)len);
    if (len < 60) { memset(b + len, 0, (size_t)(60 - len)); len = 60; }
    outl((u16)(io + 0x20 + 4 * tx_cur), (u32)(txphys + tx_cur * 2048));
    outl(tsd, (u32)len);                                               /* writing the size starts the transmission */
    tx_cur = (tx_cur + 1) & 3;
    n_tx++;
    irq_restore(fl);
    return 0;
}

static void rtl_poll(void (*cb)(const u8 *, int))
{
    while (!(inb((u16)(io + 0x37)) & 1)) {                             /* BUFE clear: a packet is waiting */
        u16 st = *(u16 *)(rxring + rx_off), len = *(u16 *)(rxring + rx_off + 2);
        if (!(st & 1) || len < 4 || len > 1600) { outb((u16)(io + 0x37), 0x04); outb((u16)(io + 0x37), 0x0C); rx_off = 0; outw((u16)(io + 0x38), (u16)(0 - 16)); outl((u16)(io + 0x44), 0x8F); break; }
        n_rx++;
        cb(rxring + rx_off + 4, len - 4);
        rx_off = (rx_off + len + 4 + 3) & ~3u;
        outw((u16)(io + 0x38), (u16)(rx_off - 16));
        rx_off %= RXLEN;
    }
}

static void rtl_stats(u64 *r, u64 *t, u64 *d) { *r = n_rx; *t = n_tx; *d = n_drop; }
const struct nic_driver nic_drv_rtl8139 = { "rtl8139", rtl_init, rtl_send, rtl_poll, rtl_mac, rtl_stats };
