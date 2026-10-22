#include "usb.h"
#include "net.h"

/*
 * xHCI host controller (USB 3.x / 2.0 ports on every PC and in QEMU/VirtualBox/VMware) driven by polling.
 * One worker thread owns the controller: it enumerates devices on the root ports, runs their class drivers and polls interrupt endpoints.
 * Not supported yet: external hubs, isochronous transfers, streams, USB 3 link power management.
 */
#define TRB_PER_RING 256
struct trb { u64 param; u32 status; u32 ctrl; } __attribute__((packed));
struct ring { struct trb *trb; u64 phys; int enq; int cycle; };

enum { T_NORMAL = 1, T_SETUP = 2, T_DATA = 3, T_STATUS = 4, T_LINK = 6, T_ENABLE_SLOT = 9, T_ADDRESS = 11, T_CONFIG_EP = 12, T_EVAL = 13, T_NOOP_CMD = 23,
       E_TRANSFER = 32, E_CMD_DONE = 33, E_PORT = 34 };

struct epstate { struct ring ring; int active; volatile int done; volatile u32 code, residual; };
struct xdev {
    struct usb_dev pub;
    u8 *dctx; u64 dctx_phys;                           /* output device context */
    u8 *ictx; u64 ictx_phys;                           /* input context (reused for every command) */
    struct epstate ep[32];                              /* by DCI (1 = control) */
    u8 maxpkt0;
};

static volatile u8 *cap, *op, *rt, *db;
static int csz64, nports, nslots;
static u64 *dcbaa; static u64 dcbaa_phys;
static struct ring cmdring, evring; static int ev_deq, ev_cycle = 1;
static struct xdev devs[USB_MAX_DEV];
static u8 *scratch; static u64 scratch_phys;            /* one DMA page for descriptors and bulk staging */
static int present, started;
#define PORT_KEEP    0x0E00C200u      /* PP, PIC, WCE/WDE/WOE: read-write bits that must be written back unchanged */
#define PORT_CHANGES 0x00FE0000u      /* CSC PEC WRC OCC PRC PLC CEC: write 1 to clear */
static struct tunable *t_enable;

static inline u32 rd32(volatile u8 *b, u32 o) { return *(volatile u32 *)(b + o); }
static inline void wr32(volatile u8 *b, u32 o, u32 v) { *(volatile u32 *)(b + o) = v; }
static inline void wr64(volatile u8 *b, u32 o, u64 v) { wr32(b, o, (u32)v); wr32(b, o + 4, (u32)(v >> 32)); }

static int ring_new(struct ring *r)
{
    u64 ph = pmm_alloc(1);
    if (!ph) return -1;
    r->trb = P2V(ph); r->phys = ph; r->enq = 0; r->cycle = 1;
    memset(r->trb, 0, 4096);
    r->trb[TRB_PER_RING - 1] = (struct trb){ ph, 0, (T_LINK << 10) | 2 | 1 };      /* link back to the start, toggle cycle */
    return 0;
}

static u64 ring_push(struct ring *r, u64 param, u32 status, u32 ctrl)
{
    struct trb *t = &r->trb[r->enq];
    u64 at = r->phys + (u64)r->enq * 16;
    t->param = param; t->status = status;
    __asm__ volatile("" ::: "memory");
    t->ctrl = ctrl | (u32)r->cycle;
    if (++r->enq == TRB_PER_RING - 1) {                  /* reached the link TRB: hand the ring over (set its cycle bit), wrap */
        r->trb[TRB_PER_RING - 1].ctrl = (r->trb[TRB_PER_RING - 1].ctrl & ~1u) | (u32)r->cycle;
        r->cycle ^= 1; r->enq = 0;
    }
    return at;
}

static void ring_db(int slot, int target) { __asm__ volatile("" ::: "memory"); wr32(db, (u32)slot * 4, (u32)target); }

/* ---- events ---- */
static volatile int cmd_done; static volatile u32 cmd_code, cmd_slot; static u64 cmd_trb;
static volatile int port_event;

static void handle_event(struct trb *e)
{
    u32 type = (e->ctrl >> 10) & 63;
    if (type == E_CMD_DONE) {
        if (e->param == cmd_trb) { cmd_code = e->status >> 24; cmd_slot = e->ctrl >> 24; cmd_done = 1; }
    } else if (type == E_TRANSFER) {
        u32 slot = e->ctrl >> 24, ep = (e->ctrl >> 16) & 31;
        for (int i = 0; i < USB_MAX_DEV; i++) if (devs[i].pub.used && (u32)devs[i].pub.slot == slot) {
            devs[i].ep[ep].code = e->status >> 24; devs[i].ep[ep].residual = e->status & 0xFFFFFF; devs[i].ep[ep].done = 1;
        }
    } else if (type == E_PORT) port_event = 1;
}

static void poll_events(void)
{
    for (;;) {
        struct trb *e = &evring.trb[ev_deq];
        if ((e->ctrl & 1) != (u32)ev_cycle) break;
        struct trb copy = *e;
        handle_event(&copy);
        if (++ev_deq == TRB_PER_RING) { ev_deq = 0; ev_cycle ^= 1; }
        wr64(rt, 0x38, (evring.phys + (u64)ev_deq * 16) | 8);                  /* ERDP, clear the event-handler-busy bit */
    }
}

static int command(u64 param, u32 status, u32 ctrl, u32 *slot_out)
{
    cmd_done = 0;
    cmd_trb = ring_push(&cmdring, param, status, ctrl);
    ring_db(0, 0);
    for (int i = 0; i < 2000 && !cmd_done; i++) { poll_events(); if (!cmd_done) thread_sleep_ms(1); }
    if (!cmd_done) return -1;
    if (slot_out) *slot_out = cmd_slot;
    return cmd_code == 1 ? 0 : -(int)cmd_code;
}

/* ---- transfers ---- */
static int transfer_wait(struct xdev *d, int dci, int ms)
{
    struct epstate *e = &d->ep[dci];
    for (int i = 0; i < ms && !e->done; i++) { poll_events(); if (!e->done) thread_sleep_ms(1); }
    if (!e->done) return -1;
    return (e->code == 1 || e->code == 13) ? 0 : -(int)e->code;
}

static int ctrl_xfer(struct xdev *d, u8 rt_, u8 req, u16 val, u16 idx, u16 len, void *buf)
{
    struct epstate *e = &d->ep[1];
    int in = rt_ & 0x80;
    u64 setup = (u64)rt_ | ((u64)req << 8) | ((u64)val << 16) | ((u64)idx << 32) | ((u64)len << 48);
    u32 trt = len ? (in ? 3 : 2) : 0;
    if (!in && len) memcpy(scratch, buf, len);
    e->done = 0;
    ring_push(&e->ring, setup, 8, (T_SETUP << 10) | (1 << 6) | (trt << 16));
    if (len) ring_push(&e->ring, scratch_phys, len, (T_DATA << 10) | (in ? (1 << 16) : 0));
    ring_push(&e->ring, 0, 0, (T_STATUS << 10) | (1 << 5) | ((in && len) ? 0 : (1 << 16)));       /* status stage opposite to data, IOC */
    ring_db(d->pub.slot, 1);
    int r = transfer_wait(d, 1, 1000);
    if (r == 0 && in && len) memcpy(buf, scratch, len);
    return r;
}

/* bulk / interrupt transfer through the shared scratch page (len <= 4096) */
static int bulk_xfer(struct xdev *d, int dci, void *buf, u32 len, int out, int ms, u32 *got)
{
    struct epstate *e = &d->ep[dci];
    if (out && len) memcpy(scratch, buf, len);
    e->done = 0;
    ring_push(&e->ring, scratch_phys, len, (T_NORMAL << 10) | (1 << 5) | (1 << 2));
    ring_db(d->pub.slot, dci);
    int r = transfer_wait(d, dci, ms);
    if (r) return r;
    u32 n = len - e->residual;
    if (!out && n) memcpy(buf, scratch, n);
    if (got) *got = n;
    return 0;
}

/* ---- context helpers ---- */
static u32 *ctx_slot(u8 *ic) { return (u32 *)(ic + (size_t)(csz64 ? 64 : 32)); }
static u32 *ctx_ep(u8 *ic, int dci) { return (u32 *)(ic + (size_t)(csz64 ? 64 : 32) * (size_t)(1 + dci)); }   /* input ctx: [control][slot][ep0=dci1]... */

static int setup_device(int port, int speed)
{
    struct xdev *d = NULL;
    for (int i = 0; i < USB_MAX_DEV; i++) if (!devs[i].pub.used) { d = &devs[i]; break; }
    if (!d) return -1;
    memset(d, 0, sizeof *d);
    u32 slot;
    if (command(0, 0, T_ENABLE_SLOT << 10, &slot)) return -1;
    d->pub.slot = (int)slot; d->pub.port = port; d->pub.speed = speed;
    u64 ph = pmm_alloc(2), pi = pmm_alloc(1);
    if (!ph || !pi) return -1;
    d->dctx = P2V(ph); d->dctx_phys = ph; d->ictx = P2V(pi); d->ictx_phys = pi;
    memset(d->dctx, 0, 8192); memset(d->ictx, 0, 4096);
    dcbaa[slot] = ph;
    if (ring_new(&d->ep[1].ring)) return -1;
    d->ep[1].active = 1;
    d->maxpkt0 = speed == 2 ? 8 : speed == 4 ? 255 : 64;               /* low-speed 8, full/high 64 (USB3: 512 = 2^9, encoded as 9 in the descriptor) */
    u32 *ctl = (u32 *)d->ictx; ctl[1] = 3;                              /* add flags: slot + EP0 */
    u32 *sl = ctx_slot(d->ictx);
    sl[0] = ((u32)speed << 20) | (1u << 27);                            /* speed, context entries = 1 */
    sl[1] = (u32)port << 16;
    u32 *e0 = ctx_ep(d->ictx, 1);
    u32 mps = speed == 4 ? 512 : d->maxpkt0;
    e0[1] = (4u << 3) | (3u << 1) | (mps << 16);                        /* control endpoint, CErr = 3 */
    e0[2] = (u32)(d->ep[1].ring.phys | 1); e0[3] = (u32)(d->ep[1].ring.phys >> 32);
    e0[4] = 8;
    if (command(d->ictx_phys, 0, (T_ADDRESS << 10) | (slot << 24), NULL)) return -1;
    d->pub.used = 1;
    return 0;
}

static void le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

/* configure the endpoints a class driver asked for. eps[i] = {dci, type(xHCI), maxpkt, interval} */
struct epcfg { int dci, type, maxpkt, interval; };
static int configure(struct xdev *d, struct epcfg *c, int n)
{
    memset(d->ictx, 0, 4096);
    u32 *ctl = (u32 *)d->ictx; u32 flags = 1; int maxdci = 1;
    for (int i = 0; i < n; i++) { flags |= 1u << c[i].dci; if (c[i].dci > maxdci) maxdci = c[i].dci; }
    ctl[1] = flags;
    u32 *sl = ctx_slot(d->ictx);
    memcpy(sl, d->dctx, 12);                                            /* the slot context the controller built (output contexts start with the slot, input contexts with a control header) */
    sl[0] = (sl[0] & 0x07FFFFFF) | ((u32)maxdci << 27);
    for (int i = 0; i < n; i++) {
        struct epstate *e = &d->ep[c[i].dci];
        if (ring_new(&e->ring)) return -1;
        e->active = 1;
        u32 *ec = ctx_ep(d->ictx, c[i].dci);
        int intr = c[i].type == 3 || c[i].type == 7;
        u32 iv = 0;
        if (intr) { u32 ms = (u32)c[i].interval ? (u32)c[i].interval : 8; iv = 3; while ((1u << iv) < ms * 8 && iv < 10) iv++; }        /* high speed intervals are in 125us units: crude log2 */
        if (intr && d->pub.speed != 3 && d->pub.speed != 4) { u32 ms = c[i].interval ? (u32)c[i].interval : 8; iv = 3; while ((1u << (iv - 3)) < ms && iv < 10) iv++; }
        ec[0] = iv << 16;
        ec[1] = ((u32)c[i].type << 3) | (3u << 1) | ((u32)c[i].maxpkt << 16);
        ec[2] = (u32)(e->ring.phys | 1); ec[3] = (u32)(e->ring.phys >> 32);
        ec[4] = intr ? (u32)c[i].maxpkt | ((u32)c[i].maxpkt << 16) : 0;
    }
    return command(d->ictx_phys, 0, (T_CONFIG_EP << 10) | ((u32)d->pub.slot << 24), NULL);
}

/* ---- enumeration ---- */
#include "usb_classes.h"

static void enumerate_port(int port)
{
    volatile u8 *pr = op + 0x400 + 0x10 * (port - 1);
    u32 sc = rd32(pr, 0);
    if (!(sc & 1)) return;                                              /* nothing connected */
    for (int i = 0; i < USB_MAX_DEV; i++) if (devs[i].pub.used && devs[i].pub.port == port) return;
    if (!(sc & 2)) {                                                    /* USB 2 ports need an explicit reset; USB 3 ports train by themselves */
        wr32(pr, 0, (sc & PORT_KEEP) | (1u << 4));                      /* PR (keep the preserved RW bits; never write 1 to PED or the change bits by accident) */
        for (int i = 0; i < 500; i++) { thread_sleep_ms(2); sc = rd32(pr, 0); if (!(sc & (1u << 4)) && (sc & 2)) break; }
        if (!(sc & 2)) return;
        thread_sleep_ms(20);
    }
    wr32(pr, 0, (rd32(pr, 0) & PORT_KEEP) | PORT_CHANGES);             /* acknowledge change bits (write-1-to-clear) */
    int speed = (int)((rd32(pr, 0) >> 10) & 15);
    if (setup_device(port, speed)) { kprintf("[usb] port %d: could not address the device\n", port); return; }
    struct xdev *d = NULL;
    for (int i = 0; i < USB_MAX_DEV; i++) if (devs[i].pub.used && devs[i].pub.port == port) d = &devs[i];
    if (!d) return;
    u8 desc[64];
    if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 18, desc)) { kprintf("[usb] port %d: no device descriptor\n", port); d->pub.used = 0; return; }
    d->pub.vid = (u16)(desc[8] | desc[9] << 8); d->pub.pid = (u16)(desc[10] | desc[11] << 8);
    d->pub.cls = desc[4];
    u8 cfg[256];
    if (ctrl_xfer(d, 0x80, 6, 0x0200, 0, 255, cfg)) { kprintf("[usb] port %d: no configuration descriptor\n", port); d->pub.used = 0; return; }
    classify(d, cfg, (int)(cfg[2] | cfg[3] << 8 > 255 ? 255 : (cfg[2] | cfg[3] << 8)), cfg[5]);
    ksnprintf(d->pub.name, sizeof d->pub.name, "%04x:%04x %s", d->pub.vid, d->pub.pid, d->pub.kind == 1 ? "keyboard" : d->pub.kind == 2 ? "mouse" : d->pub.kind == 3 ? "tablet" : d->pub.kind == 4 ? "storage" : d->pub.kind == 5 ? "network (CDC-ECM)" : "unsupported");
    kprintf("[usb] port %d: %s (class %02x)\n", port, d->pub.name, d->pub.cls);
}

static void usb_thread(void *arg)
{
    (void)arg;
    for (int i = 1; i <= nports; i++) enumerate_port(i);
    for (;;) {
        poll_events();
        if (port_event) { port_event = 0; for (int i = 1; i <= nports; i++) enumerate_port(i); }
        class_poll();
        thread_sleep_ms(2);
    }
}

void usb_init(void)
{
    t_enable = tune_bool("usb", "enable", "drive USB controllers (xHCI)", TF_BOOT, 1, NULL);
    if (!t_enable->val) return;
    struct pci_dev p; int found = -1;
    for (int i = 0; i < pci_count(); i++) { if (pci_get(i, &p)) break; if (p.class == 0x0C && p.subclass == 0x03 && ((pci_read(p.bus, p.dev, p.fn, 8) >> 8) & 0xFF) == 0x30) { found = i; break; } }
    if (found < 0) return;
    pci_get(found, &p);
    pci_write(p.bus, p.dev, p.fn, 4, pci_read(p.bus, p.dev, p.fn, 4) | 0x6);        /* memory space + bus master */
    u32 b0 = pci_read(p.bus, p.dev, p.fn, 0x10), b1 = pci_read(p.bus, p.dev, p.fn, 0x14);
    u64 bar = (u64)(b0 & ~0xFu) | ((b0 & 6) == 4 ? (u64)b1 << 32 : 0);
    if (!bar || bar >= 0x100000000ull) return;                                       /* outside the 4 GiB direct map */
    cap = (volatile u8 *)P2V(bar);
    u32 caplen = *(volatile u8 *)cap;
    op = cap + caplen; rt = cap + (rd32(cap, 0x18) & ~0x1Fu); db = cap + (rd32(cap, 0x14) & ~3u);
    u32 hcs1 = rd32(cap, 4), hcs2 = rd32(cap, 8), hcc = rd32(cap, 0x10);
    nslots = (int)(hcs1 & 0xFF); nports = (int)(hcs1 >> 24); csz64 = (hcc >> 2) & 1;
    if (nslots > 32) nslots = 32;
    /* stop, reset */
    wr32(op, 0, rd32(op, 0) & ~1u);
    for (int i = 0; i < 100 && !(rd32(op, 4) & 1); i++) thread_sleep_ms(1);
    wr32(op, 0, rd32(op, 0) | 2);
    for (int i = 0; i < 500 && (rd32(op, 0) & 2); i++) thread_sleep_ms(1);
    for (int i = 0; i < 500 && (rd32(op, 4) & (1u << 11)); i++) thread_sleep_ms(1);
    u64 pd = pmm_alloc(1), ps = pmm_alloc(1);
    if (!pd || !ps || ring_new(&cmdring) || ring_new(&evring)) return;
    memset(&evring.trb[TRB_PER_RING - 1], 0, sizeof(struct trb));                    /* an event ring is a plain array: no link TRB */
    dcbaa = P2V(pd); dcbaa_phys = pd; memset(dcbaa, 0, 4096);
    scratch = P2V(ps); scratch_phys = ps; memset(scratch, 0, 4096);
    u32 nsp = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (nsp) {                                                                       /* scratchpad buffers the controller wants for itself */
        u64 arr = pmm_alloc(1); if (!arr) return;
        u64 *a = P2V(arr); memset(a, 0, 4096);
        for (u32 i = 0; i < nsp && i < 512; i++) { u64 pg = pmm_alloc(1); if (!pg) return; memset(P2V(pg), 0, 4096); a[i] = pg; }
        dcbaa[0] = arr;
    }
    wr64(op, 0x30, dcbaa_phys);
    wr32(op, 0x38, (u32)nslots);
    wr64(op, 0x18, cmdring.phys | 1);                                                /* command ring dequeue + RCS */
    u64 erst = pmm_alloc(1); if (!erst) return;
    u64 *e = P2V(erst); memset(e, 0, 4096); e[0] = evring.phys; e[1] = TRB_PER_RING;
    wr32(rt, 0x28, 1); wr64(rt, 0x38, evring.phys); wr64(rt, 0x30, erst);
    wr32(op, 0, rd32(op, 0) | 1);                                                    /* run */
    for (int i = 0; i < 100 && (rd32(op, 4) & 1); i++) thread_sleep_ms(1);
    present = 1;
    module_register("usb", "1.0", "xHCI host controller, HID keyboard/mouse/tablet, mass storage (FAT), CDC-ECM network");
    kprintf("[usb] xHCI at %lx: %d ports, %d slots\n", bar, nports, nslots);
}

void usb_start(void) { if (present && !started) { started = 1; thread_create("usb", 5, usb_thread, NULL); } }
int usb_present(void) { return present; }
int usb_count(void) { int n = 0; for (int i = 0; i < USB_MAX_DEV; i++) n += devs[i].pub.used; return n; }
const struct usb_dev *usb_get(int idx) { int k = 0; for (int i = 0; i < USB_MAX_DEV; i++) if (devs[i].pub.used) { if (k++ == idx) return &devs[i].pub; } return NULL; }
