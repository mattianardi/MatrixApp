/* /dev/fb0: the linear framebuffer as Linux fbdev (FBIOGET_*SCREENINFO, mmap, write). While a program holds it open the workspace stops drawing. */
#include "chr.h"
#include "proc.h"
#include "vm.h"

void gui_fb_info(u64 *phys, int *w, int *h, int *pitch);
void *gui_fb_ptr(void);
extern volatile int gui_suspended;
static int holders;

static int fb_open(int m) { (void)m; if (holders++ == 0) gui_suspended = 1; return 0; }
static void fb_close(int m) { (void)m; if (holders > 0 && --holders == 0) gui_suspended = 0; }
static u64 fb_size(int m) { (void)m; u64 ph; int w, h, p; gui_fb_info(&ph, &w, &h, &p); return (u64)p * (u64)h; }

static long fb_ioctl(int m, u64 req, u64 arg, struct proc *p)
{
    (void)m; u64 ph; int w, h, pitch; gui_fb_info(&ph, &w, &h, &pitch);
    switch (req) {
    case 0x4600: case 0x4601: {                                         /* FBIOGET_VSCREENINFO / FBIOPUT_VSCREENINFO (the mode cannot change) */
        if (req == 0x4601) return 0;
        u32 v[40]; memset(v, 0, sizeof v);
        v[0] = (u32)w; v[1] = (u32)h; v[2] = (u32)w; v[3] = (u32)h; v[6] = 32;
        v[8] = 16; v[9] = 8; v[11] = 8; v[12] = 8; v[14] = 0; v[15] = 8;            /* red @16, green @8, blue @0, 8 bits each */
        v[17] = 24; v[18] = 8; v[22] = 0xFFFFFFFFu; v[23] = 0xFFFFFFFFu;            /* transp @24 (8 bits), height / width unknown */
        v[20] = 1; v[21] = 0;
        return copy_to_user(&p->as, arg, v, sizeof v) ? -E_FAULT : 0;
    }
    case 0x4602: {                                                      /* FBIOGET_FSCREENINFO */
        u8 f[80]; memset(f, 0, sizeof f);
        memcpy(f, "NEOS fb", 8);
        *(u64 *)(f + 16) = ph; *(u32 *)(f + 24) = (u32)((u64)pitch * (u64)h); *(u32 *)(f + 28) = 0; *(u32 *)(f + 36) = 2; /* FB_TYPE_PACKED_PIXELS, FB_VISUAL_TRUECOLOR */
        *(u32 *)(f + 48) = (u32)pitch;
        return copy_to_user(&p->as, arg, f, sizeof f) ? -E_FAULT : 0;
    }
    case 0x4606: case 0x4611: case 0x4604: case 0x4605: return 0;         /* FBIOPAN_DISPLAY, FBIOBLANK, FBIOGETCMAP / PUTCMAP */
    }
    return -E_NOTTY;
}
static long fb_write(int m, const void *buf, size_t n) { (void)m; (void)buf; return (long)n; }       /* write(2) at an offset is handled in the dispatcher */
static int fb_mmap(int m, struct proc *p, u64 va, u64 len, int prot, u64 off)
{
    (void)m; u64 ph; int w, h, pitch; gui_fb_info(&ph, &w, &h, &pitch);
    u64 size = (u64)pitch * (u64)h;
    if (off >= size || (off & 0xFFF)) return -E_INVAL;
    if (len > ((size - off) + 0xFFF & ~0xFFFull)) len = (size - off + 0xFFF) & ~0xFFFull;
    return vma_add_phys(&p->as, va, va + len, prot, ph + off) ? -E_NOMEM : 0;
}
static const struct chrops fbops = { .ioctl = fb_ioctl, .write = fb_write, .open = fb_open, .close = fb_close, .mmap = fb_mmap, .size = fb_size };

void fbdev_init(void)
{
    u64 ph; int w, h, p; gui_fb_info(&ph, &w, &h, &p);
    if (ph) chr_register("/dev/fb0", &fbops, 0);
}
