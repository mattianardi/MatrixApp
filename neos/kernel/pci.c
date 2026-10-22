#include "net.h"

#define MAXDEV 32
static struct pci_dev devs[MAXDEV];
static int ndev;

u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off)
{
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

void pci_scan(void)
{
    ndev = 0;
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                u32 id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (!fn) break; continue; }
                u32 cls = pci_read(bus, dev, fn, 8);
                if (ndev < MAXDEV)
                    devs[ndev++] = (struct pci_dev){ bus, dev, fn, id & 0xFFFF, id >> 16, cls >> 24, (cls >> 16) & 0xFF };
                if (!fn && !(((pci_read(bus, dev, fn, 0x0C) >> 16) & 0x80))) break;     /* single-function device */
            }
}

int pci_count(void) { return ndev; }
int pci_get(int i, struct pci_dev *o) { if (i < 0 || i >= ndev) return -1; *o = devs[i]; return 0; }
int pci_find(u16 v, u16 d, struct pci_dev *o)
{
    for (int i = 0; i < ndev; i++) if (devs[i].vendor == v && devs[i].device == d) { *o = devs[i]; return 0; }
    return -1;
}
