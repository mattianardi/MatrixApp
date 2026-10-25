#pragma once
/* NEOS window protocol: /dev/neosgfx. Every open() is one window; the program draws into a shared pixel buffer (mmap) and presents it,
 * and reads input events from the same descriptor.  Used by the Win32 layer (winrt), the browser (Chromium screencast) and any native client.
 *   ioctl(fd, NG_SETUP, &setup)   create the window: size and title; fills in stride and the screen size
 *   mmap(NULL, w*h*4, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)   the pixels, 0x00RRGGBB
 *   ioctl(fd, NG_PRESENT, 0)      show what was drawn
 *   read(fd, &ev, sizeof ev)      keyboard / pointer / close events (24 bytes each; poll() works)             */
#ifndef __KERNEL_BUILD__
#include <stdint.h>
typedef uint32_t ng_u32; typedef int32_t ng_i32;
#else
typedef u32 ng_u32; typedef int ng_i32;
#endif
struct ngsetup { ng_u32 w, h, stride, screen_w, screen_h, flags; char title[64]; };
struct ngev { ng_u32 type; ng_i32 x, y; ng_u32 code, mods, extra; };
enum { NGE_KEY = 1, NGE_DOWN, NGE_UP, NGE_MOVE, NGE_CLOSE, NGE_FOCUS, NGE_WHEEL };      /* DOWN/UP: code = button (1 left, 2 right, 4 middle); KEY: code = character or 0x100+ special; FOCUS: code = 1 gained / 0 lost */
#define NG_SETUP   0x6E01
#define NG_PRESENT 0x6E02
#define NG_TITLE   0x6E03
#define NG_MOVE    0x6E04
#define NG_INFO    0x6E05
