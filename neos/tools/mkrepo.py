#!/usr/bin/env python3
"""Build a NEOS package repository.

    tools/mkrepo.py <packages-dir> <out-dir>

Each sub-directory of <packages-dir> is a package: files + a PKGINFO (version=, description=).
Writes <out-dir>/<name>-<version>.npk and <out-dir>/index.txt, and prints the index SHA-256
(use it with `tune set pkg.index_sha256 <hash>` to pin the index when the transport is plain HTTP).
"""
import hashlib, os, sys

def build(pkgdir, name):
    info = {}
    with open(os.path.join(pkgdir, "PKGINFO")) as f:
        for line in f:
            if "=" in line:
                k, v = line.strip().split("=", 1)
                info[k] = v
    out = bytearray(b"NPK1\n")
    for root, _, files in sorted(os.walk(pkgdir)):
        for fn in sorted(files):
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, pkgdir).replace(os.sep, "/")
            if rel == "PKGINFO":
                continue
            data = open(full, "rb").read()
            out += f"F {rel} {len(data)}\n".encode() + data + b"\n"
    out += b"END\n"
    return info, bytes(out)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    lines = ["# NEOS package index v1: name version size sha256 file description"]
    for name in sorted(os.listdir(src)):
        p = os.path.join(src, name)
        if not os.path.isfile(os.path.join(p, "PKGINFO")):
            continue
        info, blob = build(p, name)
        fname = f"{name}-{info['version']}.npk"
        open(os.path.join(dst, fname), "wb").write(blob)
        lines.append(f"{name} {info['version']} {len(blob)} {hashlib.sha256(blob).hexdigest()} {fname} {info.get('description', '')}")
    index = ("\n".join(lines) + "\n").encode()
    open(os.path.join(dst, "index.txt"), "wb").write(index)
    print("index sha256:", hashlib.sha256(index).hexdigest())

main()
