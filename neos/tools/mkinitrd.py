#!/usr/bin/env python3
"""Pack files into an NPK archive unpacked into the filesystem root at boot (QEMU: -initrd).
    tools/mkinitrd.py out.npk dest/path=src/file ...      e.g.  bin/hello=build/user/hello"""
import sys
out = bytearray(b"NPK1\n")
for spec in sys.argv[2:]:
    dest, src = spec.split("=", 1)
    data = open(src, "rb").read()
    out += f"F {dest} {len(data)}\n".encode() + data + b"\n"
out += b"END\n"
open(sys.argv[1], "wb").write(out)
