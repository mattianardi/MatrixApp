#!/usr/bin/env python3
"""Append files to an NPK archive: tools/npk_add.py in.npk out.npk dest=src ..."""
import sys
d = open(sys.argv[1], 'rb').read()
assert d.endswith(b"END\n")
out = bytearray(d[:-4])
for spec in sys.argv[3:]:
    dest, src = spec.split("=", 1)
    data = open(src, 'rb').read()
    out += f"F {dest} {len(data)}\n".encode() + data + b"\n"
out += b"END\n"
open(sys.argv[2], 'wb').write(out)
