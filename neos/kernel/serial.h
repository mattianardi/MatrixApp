#pragma once
#include "kernel.h"

/* Serial ports as Linux sees them: /dev/ttyS2, /dev/ttyS3 (16550 UARTs at COM3 / COM4), /dev/ttyUSBn (FTDI, CP210x), /dev/ttyACMn (CDC-ACM).
 * Drivers push received bytes with serial_rx() and pull bytes to send with serial_tx_pull(); the file layer does termios, blocking and poll. */
#define DEV_SERIAL0 100
#define SER_MAX 8
struct serdev;
int  serial_register(const char *name, void *priv);                  /* creates /dev/<name>; returns the port number or -1 */
void serial_rx(int id, const u8 *d, int n);
int  serial_tx_pull(int id, u8 *buf, int max);
int  serial_dirty(int id);                                           /* termios / modem lines changed since the driver last looked: returns 1 once */
void serial_params(int id, u32 *baud, int *bits, int *parity, int *stop2, int *dtr, int *rts);
void *serial_priv(int id);
int  serial_count(void);
const char *serial_name(int id);

/* file layer (fdt.c / linux.c) */
long serial_read(int id, void *buf, size_t n, int nonblock);
long serial_write(int id, const void *buf, size_t n, int nonblock);
int  serial_poll(int id, int events);
long serial_ioctl(int id, u64 req, u64 arg, void *proc);            /* proc = struct proc * of the caller */
u32  serial_in_gen(int id);

/* 16550 UARTs on COM3 / COM4 */
void serial_hw_init(void);
