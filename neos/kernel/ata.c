#include "kernel.h"

/* ATA PIO (LBA28) on the primary channel, master drive. Polled, interrupts masked at the device (nIEN): no IRQ14 handler needed. */
#define DATA 0x1F0
#define ERR  0x1F1
#define CNT  0x1F2
#define LBA0 0x1F3
#define LBA1 0x1F4
#define LBA2 0x1F5
#define DEV  0x1F6
#define CMD  0x1F7
#define CTRL 0x3F6
static u32 nsect;
static volatile int busy;

static int wait_ready(int need_drq)
{
    for (int i = 0; i < 2000000; i++) {
        u8 s = inb(CMD);
        if (s & 1) return -1;                                  /* ERR */
        if (!(s & 0x80) && (!need_drq || (s & 8))) return 0;
    }
    return -1;
}

int ata_init(void)
{
    outb(CTRL, 2);
    outb(DEV, 0xA0);
    for (int i = 0; i < 4; i++) inb(CTRL);                     /* 400 ns */
    outb(CNT, 0); outb(LBA0, 0); outb(LBA1, 0); outb(LBA2, 0);
    outb(CMD, 0xEC);                                           /* IDENTIFY */
    if (!inb(CMD)) return -1;                                  /* no drive */
    for (int i = 0; i < 2000000 && (inb(CMD) & 0x80); i++) { }
    if (inb(LBA1) || inb(LBA2)) return -1;                     /* ATAPI / SATA signature: not for us */
    if (wait_ready(1)) return -1;
    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(DATA);
    nsect = (u32)id[60] | ((u32)id[61] << 16);
    return nsect ? 0 : -1;
}

u32 ata_sectors(void) { return nsect; }

static int rw(u32 lba, u32 n, void *buf, int write)
{
    if (!nsect || lba + n > nsect) return -1;
    u8 *p = buf; int rc = 0;
    while (__sync_lock_test_and_set(&busy, 1)) thread_sleep_ms(1);
    while (n && !rc) {
        u32 c = n > 128 ? 128 : n;
        outb(DEV, 0xE0 | ((lba >> 24) & 0xF));
        outb(CNT, (u8)c); outb(LBA0, (u8)lba); outb(LBA1, (u8)(lba >> 8)); outb(LBA2, (u8)(lba >> 16));
        outb(CMD, write ? 0x30 : 0x20);
        for (u32 s = 0; s < c; s++) {
            if (wait_ready(1)) { rc = -1; break; }
            if (write) { for (int i = 0; i < 256; i++) outw(DATA, ((u16 *)p)[i]); }
            else { for (int i = 0; i < 256; i++) ((u16 *)p)[i] = inw(DATA); }
            p += 512;
        }
        if (!rc && write) { outb(CMD, 0xE7); rc = wait_ready(0); }       /* flush cache */
        lba += c; n -= c;
    }
    __sync_lock_release(&busy);
    return rc;
}
int ata_read(u32 lba, u32 n, void *buf) { return rw(lba, n, buf, 0); }
int ata_write(u32 lba, u32 n, const void *buf) { return rw(lba, n, (void *)buf, 1); }
