#pragma once
#include "kernel.h"

/* USB host stack: xHCI controller driver + class drivers (HID keyboard/mouse/tablet, mass storage, CDC-ECM network). Polled, one thread. */
#define USB_MAX_DEV 8
struct usb_dev {
    int used, slot, port, speed;                       /* speed: 1 full, 2 low, 3 high, 4 super */
    u16 vid, pid; u8 cls, subcls, proto;
    char name[40];
    int kind;                                          /* 0 unknown, 1 kbd, 2 mouse, 3 tablet, 4 storage, 5 ecm */
    void *priv;
};
void usb_init(void);                                   /* find xHCI controllers (PCI) */
void usb_start(void);                                  /* worker thread */
int  usb_count(void);
const struct usb_dev *usb_get(int i);
int  usb_present(void);

/* input.c sinks for HID class drivers */
void input_push_key(int ch);
void input_mouse_rel(int dx, int dy, int btn);
void input_mouse_abs(int x, int y, int max, int btn);

/* storage: 512-byte logical blocks (first mass-storage device) */
int  usb_blk_present(void);
u64  usb_blk_sectors(void);
int  usb_blk_read(u64 lba, u32 n, void *buf);
int  usb_blk_write(u64 lba, u32 n, const void *buf);

/* fat.c: mount the FAT volume of the USB stick into /mnt/usb (RAM copy), write changes back */
int  fat_mount(const char *where, const char **err);
int  fat_sync(const char **err);
void fat_umount(void);
