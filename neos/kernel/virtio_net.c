#include "net.h"

/* virtio-net, legacy (transitional) PCI interface over I/O ports: what QEMU/KVM, Firecracker-style and most hypervisors present
 * by default. Two virtqueues (0 = receive, 1 = transmit), no offloads: plain Ethernet frames behind a 10-byte header. */
#define VQ_RX 0
#define VQ_TX 1
#define NBUF 16
#define BUFSZ 2048
#define HDR 10
struct vdesc { u64 addr; u32 len; u16 flags, next; } __attribute__((packed));
struct vq { u16 qs; struct vdesc *desc; volatile u16 *avail; volatile u8 *used; u16 last_used; u16 avail_idx; u8 *buf; u64 bufphys; };

static u16 io;
static struct vq q[2];
static u8 mac[6];
static u64 n_rx, n_tx, n_drop;
static u16 tx_free[NBUF]; static int ntx_free;

static int vq_setup(int idx, struct vq *v)
{
    outw((u16)(io + 14), (u16)idx);
    v->qs = inw((u16)(io + 12));
    if (!v->qs || v->qs > 1024) return -1;
    size_t a = 16u * v->qs + 6 + 2u * v->qs, b = 6 + 8u * v->qs;
    size_t first = (a + 4095) & ~4095ul, total = first + ((b + 4095) & ~4095ul);
    u64 ph = pmm_alloc(total / 4096);
    if (!ph) return -1;
    memset(P2V(ph), 0, total);
    v->desc = P2V(ph);
    v->avail = (volatile u16 *)((u8 *)v->desc + 16u * v->qs);
    v->used = (volatile u8 *)P2V(ph) + first;
    v->bufphys = pmm_alloc(NBUF * BUFSZ / 4096);
    if (!v->bufphys) return -1;
    v->buf = P2V(v->bufphys);
    outl((u16)(io + 8), (u32)(ph >> 12));
    return 0;
}

static void post_rx(int id)
{
    struct vq *v = &q[VQ_RX];
    v->desc[id] = (struct vdesc){ v->bufphys + (u64)id * BUFSZ, BUFSZ, 2, 0 };           /* device-writable */
    v->avail[2 + (v->avail_idx % v->qs)] = (u16)id;
    __asm__ volatile("" ::: "memory");
    v->avail_idx++;
    v->avail[1] = v->avail_idx;
}

static int virtio_init(void)
{
    struct pci_dev d;
    if (pci_find(0x1AF4, 0x1000, &d)) return -1;                       /* transitional virtio-net */
    pci_write(d.bus, d.dev, d.fn, 4, pci_read(d.bus, d.dev, d.fn, 4) | 0x5);        /* I/O space + bus master */
    u32 bar = pci_read(d.bus, d.dev, d.fn, 0x10);
    if (!(bar & 1)) return -1;
    io = (u16)(bar & ~3u);
    outb((u16)(io + 18), 0);                                            /* reset */
    outb((u16)(io + 18), 1 | 2);                                        /* ACKNOWLEDGE | DRIVER */
    u32 host = inl(io);
    outl((u16)(io + 4), host & (1u << 5));                              /* only the MAC feature */
    if (vq_setup(VQ_RX, &q[VQ_RX]) || vq_setup(VQ_TX, &q[VQ_TX])) return -1;
    for (int i = 0; i < 6; i++) mac[i] = (host & (1u << 5)) ? inb((u16)(io + 20 + i)) : (u8)(0x52 + i);
    for (int i = 0; i < NBUF; i++) post_rx(i);
    for (int i = 0; i < NBUF; i++) tx_free[ntx_free++] = (u16)i;
    outb((u16)(io + 18), 1 | 2 | 4);                                    /* DRIVER_OK */
    outw((u16)(io + 16), VQ_RX);                                        /* tell the device receive buffers are available */
    return 0;
}

static const u8 *virtio_mac(void) { return mac; }

static void reclaim_tx(void)
{
    struct vq *v = &q[VQ_TX];
    while (v->last_used != *(volatile u16 *)(v->used + 2)) {
        u32 id = *(volatile u32 *)(v->used + 4 + 8u * (v->last_used % v->qs));
        if (ntx_free < NBUF) tx_free[ntx_free++] = (u16)id;
        v->last_used++;
    }
}

static int virtio_send(const u8 *frame, int len)
{
    if (len > BUFSZ - HDR) return -1;
    u64 fl = irq_save();
    reclaim_tx();
    if (!ntx_free) { n_drop++; irq_restore(fl); return -1; }
    struct vq *v = &q[VQ_TX];
    int id = tx_free[--ntx_free];
    u8 *b = v->buf + (size_t)id * BUFSZ;
    memset(b, 0, HDR); memcpy(b + HDR, frame, (size_t)len);
    v->desc[id] = (struct vdesc){ v->bufphys + (u64)id * BUFSZ, (u32)(HDR + len), 0, 0 };
    v->avail[2 + (v->avail_idx % v->qs)] = (u16)id;
    __asm__ volatile("" ::: "memory");
    v->avail_idx++;
    v->avail[1] = v->avail_idx;
    outw((u16)(io + 16), VQ_TX);
    n_tx++;
    irq_restore(fl);
    return 0;
}

static void virtio_poll(void (*cb)(const u8 *, int))
{
    struct vq *v = &q[VQ_RX];
    int kicked = 0;
    while (v->last_used != *(volatile u16 *)(v->used + 2)) {
        u32 id = *(volatile u32 *)(v->used + 4 + 8u * (v->last_used % v->qs));
        u32 len = *(volatile u32 *)(v->used + 8 + 8u * (v->last_used % v->qs));
        v->last_used++;
        if (id < NBUF && len > HDR) { n_rx++; cb(v->buf + (size_t)id * BUFSZ + HDR, (int)(len - HDR)); }
        if (id < NBUF) { post_rx((int)id); kicked = 1; }
    }
    if (kicked) outw((u16)(io + 16), VQ_RX);
}

static void virtio_stats(u64 *r, u64 *t, u64 *d) { *r = n_rx; *t = n_tx; *d = n_drop; }
const struct nic_driver nic_drv_virtio = { "virtio-net", virtio_init, virtio_send, virtio_poll, virtio_mac, virtio_stats };
