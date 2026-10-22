#include "net.h"

/* Network interface drivers register here; the stack talks to whichever one was found first (net.nic names it). */
extern const struct nic_driver nic_drv_virtio, nic_drv_e1000, nic_drv_rtl8139;
static const struct nic_driver *const drivers[] = { &nic_drv_virtio, &nic_drv_e1000, &nic_drv_rtl8139 };
static const struct nic_driver *active;

int nic_probe(void)
{
    for (unsigned i = 0; i < sizeof drivers / sizeof *drivers; i++) if (drivers[i]->init() == 0) { active = drivers[i]; return 0; }
    return -1;
}
const char *nic_name(void) { return active ? active->name : "none"; }
int nic_send(const u8 *f, int n) { return active ? active->send(f, n) : -1; }
void nic_poll(void (*cb)(const u8 *, int)) { if (active) active->poll(cb); }
const u8 *nic_mac(void) { static const u8 zero[6]; return active ? active->mac() : zero; }
void nic_stats(u64 *r, u64 *t, u64 *d) { if (active) active->stats(r, t, d); else *r = *t = *d = 0; }
