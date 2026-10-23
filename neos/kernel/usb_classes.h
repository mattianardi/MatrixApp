/* Class drivers, compiled into usb_xhci.c (they share the controller's transfer helpers). */

struct hid { u8 *buf; u64 buf_phys; int dci, maxpkt, abs; u8 prev[8]; };
struct msc { int in_dci, out_dci; u64 sectors; u32 blksz; u32 tag; int ready; };
struct blkreq { volatile int pending, done; int write, rc; u64 lba; u32 n; void *buf; };
static struct blkreq blk_req; static struct xdev *blk_dev;

/* ---------------------------------------------------------------- HID */
static int hid_key(u8 usage, int shift, int ctrl)
{
    static const char plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
    static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
    int c = 0;
    if (usage >= 0x04 && usage <= 0x38) c = (shift ? shifted : plain)[usage - 4];
    else switch (usage) { case 0x4F: return 0x103; case 0x50: return 0x102; case 0x51: return 0x101; case 0x52: return 0x100; case 0x4A: return 0x104; case 0x4D: return 0x105; case 0x4C: return 0x106; case 0x4B: return 0x107; case 0x4E: return 0x108; default: return 0; }
    if (ctrl && is_alpha(c)) c = to_lower(c) - 'a' + 1;
    return c;
}

static void hid_arm(struct xdev *d, struct hid *h)
{
    struct epstate *e = &d->ep[h->dci];
    e->done = 0;
    ring_push(&e->ring, h->buf_phys, (u32)h->maxpkt, (T_NORMAL << 10) | (1 << 5) | (1 << 2));
    ring_db(d->pub.slot, h->dci);
}

static void hid_report(struct xdev *d, struct hid *h)
{
    u8 *r = h->buf;
    if (d->pub.kind == 1) {
        int shift = (r[0] & 0x22) != 0, ctrl = (r[0] & 0x11) != 0;
        for (int i = 2; i < 8; i++) {
            if (!r[i] || r[i] == 1) continue;
            int seen = 0; for (int k = 2; k < 8; k++) if (h->prev[k] == r[i]) seen = 1;
            if (seen) continue;
            int c = hid_key(r[i], shift, ctrl);
            if (c) input_push_key(c);
        }
        memcpy(h->prev, r, 8);
    } else if (d->pub.kind == 2) {
        input_mouse_rel((signed char)r[1], (signed char)r[2], r[0] & 7);
    } else if (d->pub.kind == 3) {                                      /* QEMU usb-tablet: buttons, x (16), y (16), wheel */
        input_mouse_abs(r[1] | r[2] << 8, r[3] | r[4] << 8, 0x7FFF, r[0] & 7);
    }
}

/* ---------------------------------------------------------------- mass storage (bulk-only transport, SCSI transparent) */
static int msc_cmd(struct xdev *d, struct msc *m, const u8 *cdb, int cdblen, int in, void *buf, u32 len)
{
    u8 cbw[31]; memset(cbw, 0, sizeof cbw);
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    u32 tag = ++m->tag; memcpy(cbw + 4, &tag, 4); memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0; cbw[14] = (u8)cdblen; memcpy(cbw + 15, cdb, (size_t)cdblen);
    int r;
    if ((r = bulk_xfer(d, m->out_dci, cbw, 31, 1, 2000, NULL))) { kprintf("[usb] msc: CBW failed (%d)\n", r); return -1; }
    if (len) { u32 got; if ((r = bulk_xfer(d, in ? m->in_dci : m->out_dci, buf, len, !in, 5000, &got))) { kprintf("[usb] msc: data stage failed (%d) cdb %02x len %u tag %u in-enq %d cyc %d\n", r, cdb[0], len, tag, d->ep[m->in_dci].ring.enq, d->ep[m->in_dci].ring.cycle); return -2; } }
    u8 csw[13]; u32 got;
    if ((r = bulk_xfer(d, m->in_dci, csw, 13, 0, 2000, &got)) || got != 13 || csw[0] != 'U' || csw[1] != 'S') { kprintf("[usb] msc: CSW failed (%d, %u bytes)\n", r, got); return -3; }
    if (csw[12]) kprintf("[usb] msc: command %02x status %d\n", cdb[0], csw[12]);
    return csw[12] ? -4 : 0;
}

static int msc_init(struct xdev *d, struct msc *m)
{
    u8 buf[64];
    static const u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 }, tur[6] = { 0 }, cap[10] = { 0x25 };
    msc_cmd(d, m, inq, 6, 1, buf, 36);
    int ok = -1;
    for (int i = 0; i < 20 && ok; i++) { ok = msc_cmd(d, m, tur, 6, 0, NULL, 0); if (ok) thread_sleep_ms(50); }
    if (msc_cmd(d, m, cap, 10, 1, buf, 8)) return -1;
    u32 last = (u32)buf[0] << 24 | (u32)buf[1] << 16 | (u32)buf[2] << 8 | buf[3];
    m->blksz = (u32)buf[4] << 24 | (u32)buf[5] << 16 | (u32)buf[6] << 8 | buf[7];
    m->sectors = (u64)last + 1;
    return m->blksz == 512 ? 0 : -1;
}

static int msc_rw(struct xdev *d, struct msc *m, int write, u64 lba, u32 n, void *buf)
{
    u8 *p = buf;
    while (n) {
        u32 c = n > 8 ? 8 : n;                                          /* 8 x 512 = one scratch page */
        u8 cdb[10] = { write ? 0x2A : 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, (u8)(c >> 8), (u8)c, 0 };
        if (msc_cmd(d, m, cdb, 10, !write, p, c * 512)) return -1;
        p += c * 512; lba += c; n -= c;
    }
    return 0;
}


/* ---------------------------------------------------------------- USB serial: CDC-ACM (kind 6), FTDI (kind 7), CP210x (kind 8) */
struct usbser { int kind, in_dci, out_dci, maxin, comm_iface, id; u8 *buf; u64 buf_phys; };
static void ser_arm(struct xdev *d, struct usbser *s)
{
    struct epstate *e = &d->ep[s->in_dci];
    e->done = 0;
    ring_push(&e->ring, s->buf_phys, (u32)s->maxin, (T_NORMAL << 10) | (1 << 5) | (1 << 2));
    ring_db(d->pub.slot, s->in_dci);
}
static u32 ftdi_divisor(u32 baud)                                       /* libftdi's ftdi_to_clkbits(): 48 MHz clock, /16 */
{
    static const u8 frac_code[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
    const u32 clk = 48000000, clk_div = 16;
    if (baud >= clk / clk_div) return 0;
    if (baud >= clk / (clk_div + clk_div / 2)) return 1;
    if (baud >= clk / (2 * clk_div)) return 2;
    u32 divisor = clk * 16 / clk_div / baud, best = (divisor & 1) ? divisor / 2 + 1 : divisor / 2;
    if (best > 0x20000) best = 0x1ffff;
    return (best >> 3) | ((u32)frac_code[best & 7] << 14);
}
static void ser_apply(struct xdev *d, struct usbser *s)
{
    u32 baud; int bits, par, stop2, dtr, rts;
    serial_params(s->id, &baud, &bits, &par, &stop2, &dtr, &rts);
    if (!baud) baud = 9600;
    if (s->kind == 7) {
        u32 enc = ftdi_divisor(baud);
        ctrl_xfer(d, 0x40, 3, (u16)(enc & 0xFFFF), (u16)(enc >> 16), 0, NULL);                        /* SIO_SET_BAUD_RATE */
        ctrl_xfer(d, 0x40, 4, (u16)(bits | (par == 1 ? 0x100 : par == 2 ? 0x200 : 0) | (stop2 ? 0x1000 : 0)), 0, 0, NULL);   /* SIO_SET_DATA */
        ctrl_xfer(d, 0x40, 1, (u16)((dtr ? 0x0101 : 0x0100) | (rts ? 0x0202 : 0x0200)), 0, 0, NULL);  /* SIO_SET_MODEM_CTRL */
    } else if (s->kind == 6) {
        u8 lc[7] = { (u8)baud, (u8)(baud >> 8), (u8)(baud >> 16), (u8)(baud >> 24), (u8)(stop2 ? 2 : 0), (u8)par, (u8)bits };
        ctrl_xfer(d, 0x21, 0x20, 0, (u16)s->comm_iface, 7, lc);                                       /* SET_LINE_CODING */
        ctrl_xfer(d, 0x21, 0x22, (u16)((dtr ? 1 : 0) | (rts ? 2 : 0)), (u16)s->comm_iface, 0, NULL);   /* SET_CONTROL_LINE_STATE */
    } else {
        u8 b4[4] = { (u8)baud, (u8)(baud >> 8), (u8)(baud >> 16), (u8)(baud >> 24) };
        ctrl_xfer(d, 0x41, 0x1E, 0, (u16)s->comm_iface, 4, b4);                                       /* SET_BAUDRATE */
        ctrl_xfer(d, 0x41, 0x03, (u16)(bits << 8 | par << 4 | (stop2 ? 2 : 0)), (u16)s->comm_iface, 0, NULL);   /* SET_LINE_CTL */
        ctrl_xfer(d, 0x41, 0x07, (u16)((dtr ? 1 : 0) | (rts ? 2 : 0) | 0x0300), (u16)s->comm_iface, 0, NULL);  /* SET_MHS */
    }
}
static void ser_poll(struct xdev *d, struct usbser *s)
{
    if (serial_dirty(s->id)) ser_apply(d, s);
    struct epstate *e = &d->ep[s->in_dci];
    if (e->done) {
        if (e->code == 1 || e->code == 13) {
            int n = s->maxin - (int)e->residual;
            if (s->kind == 7) {                                         /* FTDI: two modem-status bytes open every packet */
                for (int off = 0; off < n; off += 64) { int m = n - off < 64 ? n - off : 64; if (m > 2) serial_rx(s->id, s->buf + off + 2, m - 2); }
            } else if (n > 0) serial_rx(s->id, s->buf, n);
        }
        ser_arm(d, s);
    }
    u8 t[64]; int k = serial_tx_pull(s->id, t, 64);
    if (k) bulk_xfer(d, s->out_dci, t, (u32)k, 1, 200, NULL);
}

/* ---------------------------------------------------------------- classification of a configuration descriptor */
static void classify(struct xdev *d, u8 *cfg, int total, int cfgval)
{
    int iface = -1, icls = 0, isub = 0, iproto = 0, ep_in = 0, ep_out = 0, ep_in_mps = 0, ep_out_mps = 0, ep_in_iv = 0, ep_in_type = 0, ep_out_type = 0, found = 0;
    int ecm_mac_idx = 0, comm_iface = -1, comm_sub = 0;
    for (int i = 0; i + 2 <= total && cfg[i];) {
        u8 len = cfg[i], type = cfg[i + 1];
        if (type == 4 && len >= 9) {
            if (found) break;                                           /* first interface that is one of ours wins */
            iface = cfg[i + 2]; icls = cfg[i + 5]; isub = cfg[i + 6]; iproto = cfg[i + 7];
            ep_in = ep_out = 0;
            if (icls == 2) { comm_iface = iface; comm_sub = isub; }
        } else if (type == 5 && len >= 7) {
            int addr = cfg[i + 2], attr = cfg[i + 3] & 3, mps = cfg[i + 4] | cfg[i + 5] << 8, iv = cfg[i + 6];
            if (addr & 0x80) { if (!ep_in || attr == 3) { ep_in = addr; ep_in_mps = mps; ep_in_iv = iv; ep_in_type = attr; } }
            else if (!ep_out) { ep_out = addr; ep_out_mps = mps; ep_out_type = attr; }
        } else if (type == 0x24 && len >= 5 && cfg[i + 2] == 0x0F) ecm_mac_idx = cfg[i + 3];
        /* an interface is complete once its endpoints have been seen: decide at the next interface or the end */
        i += len;
        int end = i + 2 > total || !cfg[i] || cfg[i + 1] == 4;
        if (!end || iface < 0) continue;
        if (icls == 3 && isub == 1 && ep_in && (iproto == 1 || iproto == 2)) {                 /* HID boot keyboard / mouse */
            d->pub.kind = iproto == 1 ? 1 : 2; found = 1;
        } else if (icls == 3 && ep_in && d->pub.vid == 0x0627 && d->pub.pid == 0x0001) {       /* QEMU's absolute pointer */
            d->pub.kind = 3; found = 1;
        } else if (icls == 8 && isub == 6 && iproto == 0x50 && ep_in && ep_out) { d->pub.kind = 4; found = 1; }
        else if (icls == 10 && ep_in_type == 2 && ep_out_type == 2 && comm_sub == 2) { d->pub.kind = 6; found = 1; }         /* CDC data interface of an ACM modem */
        else if (icls == 10 && ep_in_type == 2 && ep_out_type == 2) { d->pub.kind = 5; found = 1; (void)ecm_mac_idx; }
        else if (icls == 0xFF && ep_in_type == 2 && ep_out_type == 2 && d->pub.vid == 0x0403) { d->pub.kind = 7; found = 1; }  /* FTDI */
        else if (icls == 0xFF && ep_in_type == 2 && ep_out_type == 2 && d->pub.vid == 0x10C4) { d->pub.kind = 8; found = 1; }  /* Silicon Labs CP210x */
        if (!found) { iface = -1; continue; }
        /* set up the endpoints and start the driver */
        if (ctrl_xfer(d, 0x00, 9, (u16)cfgval, 0, 0, NULL)) { d->pub.kind = 0; return; }
        if (d->pub.kind <= 3) {
            struct hid *h = kmalloc(sizeof *h); if (!h) { d->pub.kind = 0; return; }
            memset(h, 0, sizeof *h);
            u64 pg = pmm_alloc(1); if (!pg) { d->pub.kind = 0; return; }
            h->buf = P2V(pg); h->buf_phys = pg; memset(h->buf, 0, 64);
            h->dci = (ep_in & 15) * 2 + 1; h->maxpkt = ep_in_mps > 64 ? 64 : ep_in_mps;
            struct epcfg c1 = { h->dci, 7, ep_in_mps, ep_in_iv };
            if (configure(d, &c1, 1)) { d->pub.kind = 0; return; }
            if (d->pub.kind <= 2) { ctrl_xfer(d, 0x21, 0x0B, 0, (u16)iface, 0, NULL); ctrl_xfer(d, 0x21, 0x0A, 0, (u16)iface, 0, NULL); }
            d->pub.priv = h;
            hid_arm(d, h);
        } else if (d->pub.kind == 4) {
            struct msc *m = kmalloc(sizeof *m); if (!m) { d->pub.kind = 0; return; }
            memset(m, 0, sizeof *m);
            m->in_dci = (ep_in & 15) * 2 + 1; m->out_dci = (ep_out & 15) * 2;
            struct epcfg c2[2] = { { m->in_dci, 6, ep_in_mps, 0 }, { m->out_dci, 2, ep_out_mps, 0 } };
            if (configure(d, c2, 2)) { d->pub.kind = 0; kfree(m); return; }
            d->pub.priv = m;
            if (msc_init(d, m)) { kprintf("[usb] storage: not ready or unsupported block size\n"); d->pub.kind = 0; return; }
            m->ready = 1; blk_dev = d;
        } else if (d->pub.kind == 5) {
            d->pub.kind = 0;                                            /* CDC-ECM networking: see usb_ecm.c once present */
        } else if (d->pub.kind >= 6) {
            struct usbser *s = kmalloc(sizeof *s); if (!s) { d->pub.kind = 0; return; }
            memset(s, 0, sizeof *s);
            u64 pg = pmm_alloc(1); if (!pg) { kfree(s); d->pub.kind = 0; return; }
            s->buf = P2V(pg); s->buf_phys = pg; s->kind = d->pub.kind;
            s->in_dci = (ep_in & 15) * 2 + 1; s->out_dci = (ep_out & 15) * 2; s->maxin = ep_in_mps > 512 ? 512 : ep_in_mps;
            s->comm_iface = d->pub.kind == 6 ? comm_iface : 0;
            struct epcfg c2[2] = { { s->in_dci, 6, ep_in_mps, 0 }, { s->out_dci, 2, ep_out_mps, 0 } };
            if (configure(d, c2, 2)) { d->pub.kind = 0; kfree(s); return; }
            static int n_usb, n_acm;
            char nm[16]; ksnprintf(nm, sizeof nm, d->pub.kind == 6 ? "ttyACM%d" : "ttyUSB%d", d->pub.kind == 6 ? n_acm++ : n_usb++);
            s->id = serial_register(nm, s);
            if (s->id < 0) { d->pub.kind = 0; kfree(s); return; }
            d->pub.priv = s;
            if (d->pub.kind == 7) { ctrl_xfer(d, 0x40, 0, 0, 0, 0, NULL); }                                   /* SIO_RESET */
            else if (d->pub.kind == 8) { ctrl_xfer(d, 0x41, 0x00, 1, 0, 0, NULL); }                           /* IFC_ENABLE */
            ser_apply(d, s);
            ser_arm(d, s);
            kprintf("[usb] serial port /dev/%s\n", nm);
        }
        return;
    }
}

static void class_poll(void)
{
    for (int i = 0; i < USB_MAX_DEV; i++) {
        struct xdev *d = &devs[i];
        if (!d->pub.used || !d->pub.priv) continue;
        if (d->pub.kind >= 6) { ser_poll(d, d->pub.priv); continue; }
        if (d->pub.kind >= 1 && d->pub.kind <= 3) {
            struct hid *h = d->pub.priv;
            if (d->ep[h->dci].done) { if (d->ep[h->dci].code == 1 || d->ep[h->dci].code == 13) hid_report(d, h); hid_arm(d, h); }
        }
    }
    if (blk_req.pending && blk_dev) {                                    /* a block request from another thread, executed here: the controller is single-owner */
        struct msc *m = blk_dev->pub.priv;
        blk_req.rc = msc_rw(blk_dev, m, blk_req.write, blk_req.lba, blk_req.n, blk_req.buf);
        blk_req.pending = 0; blk_req.done = 1;
    }
}

int usb_blk_present(void) { return blk_dev != NULL; }
u64 usb_blk_sectors(void) { return blk_dev ? ((struct msc *)blk_dev->pub.priv)->sectors : 0; }
static int blk_do(int write, u64 lba, u32 n, void *buf)
{
    if (!blk_dev) return -1;
    u64 fl = irq_save();
    while (blk_req.pending || (blk_req.done && 0)) { irq_restore(fl); thread_sleep_ms(1); fl = irq_save(); }
    blk_req.write = write; blk_req.lba = lba; blk_req.n = n; blk_req.buf = buf; blk_req.done = 0; blk_req.pending = 1;
    irq_restore(fl);
    for (int i = 0; i < 20000 && !blk_req.done; i++) thread_sleep_ms(1);
    return blk_req.done ? blk_req.rc : -1;
}
int usb_blk_read(u64 lba, u32 n, void *buf) { return blk_do(0, lba, n, buf); }
int usb_blk_write(u64 lba, u32 n, const void *buf) { return blk_do(1, lba, n, (void *)buf); }
