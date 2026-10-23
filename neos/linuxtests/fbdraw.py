"""A fbdev + evdev client the way Qt's linuxfb/evdev plugins or SDL use them: query the mode, mmap the framebuffer, paint it, read key and pointer events."""
import os, mmap, struct, fcntl, time, select

fd = os.open("/dev/fb0", os.O_RDWR)
vs = fcntl.ioctl(fd, 0x4600, bytes(160)); xres, yres = struct.unpack_from("II", vs, 0); bpp = struct.unpack_from("I", vs, 24)[0]
rl, rr = struct.unpack_from("II", vs, 32)
fs = fcntl.ioctl(fd, 0x4602, bytes(80)); slen = struct.unpack_from("I", fs, 24)[0]; line = struct.unpack_from("I", fs, 48)[0]
print("FB mode", xres, yres, bpp, "line", line, "len", slen, "red", rl, rr, flush=True)
m = mmap.mmap(fd, slen, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
half = line * (yres // 2)
m[0:half] = b"\x00\x00\xff\x00" * (half // 4)                    # top half red   (0x00RRGGBB little endian: B G R X)
m[half:slen] = b"\xff\x00\x00\x00" * ((slen - half) // 4)        # bottom half blue
m.flush()
ek = os.open("/dev/input/event0", os.O_RDONLY | os.O_NONBLOCK)
em = os.open("/dev/input/event1", os.O_RDONLY | os.O_NONBLOCK)
name = fcntl.ioctl(ek, 0x80FF4506, bytes(255)).split(b"\0")[0]
print("FB ready", name.decode(), flush=True)
got_key = got_rel = False; t0 = time.time()
while time.time() - t0 < 60 and not (got_key and got_rel):
    r, _, _ = select.select([ek, em], [], [], 1)
    for f in r:
        for off in range(0, len(d := os.read(f, 24 * 16)), 24):
            sec, usec, typ, code, val = struct.unpack_from("qqHHi", d, off)
            if typ == 1 and f == ek and val == 1: print("EV key", code, flush=True); got_key = True
            if typ == 2 and f == em: print("EV rel", code, val, flush=True); got_rel = True
print("FB done", flush=True)
