#!/usr/bin/env python3
"""Build build/chrome.npk: Chromium's headless shell (the real engine: V8, Blink, Skia) plus the shared libraries it needs and a few fonts,
as an NPK that NEOS unpacks into /opt/chrome at boot (.pkginfo says extract=root).  Run NEOS with this as an extra boot module and >= 2 GiB of RAM.
    tools/mkchrome.py [--src <dir with headless_shell.real>] [--out build/chrome.npk]
Libraries come from this host (ldd); the C library itself is NEOS's own glibc package (pkg install glibc)."""
import argparse, os, re, subprocess, sys, glob
ap = argparse.ArgumentParser()
ap.add_argument('--src', default=(glob.glob('/opt/pw-browsers/chromium_headless_shell-*/chrome-linux') or [''])[0])
ap.add_argument('--out', default='build/chrome.npk')
a = ap.parse_args()
exe = os.path.join(a.src, 'headless_shell.real')
if not os.path.exists(exe): sys.exit(f'{exe}: not found (pass --src)')
PROVIDED = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1', 'libutil.so.1', 'libresolv.so.2', 'libgcc_s.so.1', 'libz.so.1', 'libstdc++.so.6', 'ld-linux-x86-64.so.2'}
libs = {}
def closure(path):
    out = subprocess.run(['ldd', path], capture_output=True, text=True).stdout
    for m in re.finditer(r'(\S+) => (/\S+)', out):
        so, real = m.group(1), m.group(2)
        if so in PROVIDED or so in libs: continue
        libs[so] = os.path.realpath(real); closure(real)
closure(exe)
for extra in ('libEGL.so', 'libGLESv2.so', 'libvk_swiftshader.so', 'libvulkan.so.1'):
    p = os.path.join(a.src, extra)
    if os.path.exists(p): libs.setdefault(extra, p)
files = {}
for f in os.listdir(a.src):
    p = os.path.join(a.src, f)
    if f == 'headless_shell': continue
    if os.path.isfile(p) and not f.endswith('.so') and '.so.' not in f: files['opt/chrome/' + f] = p
for so, real in libs.items():
    files['opt/chrome/lib/' + so] = real if not real.startswith(a.src) else real
for font in ('DejaVuSans.ttf', 'DejaVuSans-Bold.ttf', 'DejaVuSansMono.ttf', 'DejaVuSerif.ttf'):
    for root in ('/usr/share/fonts/truetype/dejavu',):
        p = os.path.join(root, font)
        if os.path.exists(p): files['usr/share/fonts/truetype/dejavu/' + font] = p
files['opt/chrome/fonts.conf'] = None
conf = b'<?xml version="1.0"?><fontconfig><dir>/usr/share/fonts</dir></fontconfig>\n'
info = b'name=chrome\nversion=1\ndescription=Chromium headless shell\nextract=root\n'
out = bytearray(b'NPK1\n')
def add(dest, data): out.extend(f'F {dest} {len(data)}\n'.encode() + data + b'\n')
add('.pkginfo', info)
for dest, src in sorted(files.items()):
    add(dest, conf if src is None else open(src, 'rb').read())
out += b'END\n'
os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
open(a.out, 'wb').write(out)
print(f'{a.out}: {len(files)} files, {len(out) >> 20} MiB; libraries: {len(libs)}')
