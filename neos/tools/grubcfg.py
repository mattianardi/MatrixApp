#!/usr/bin/env python3
"""GRUB menu for the ISO: normal, safe (text), and lite (text, without the offline package cache) - with progress messages while the files load.
usage: grubcfg.py <cache.npk ...>"""
import os, sys
cache = [os.path.basename(f) for f in sys.argv[1:]]
def entry(title, gfx, mods, note):
    s = f'menuentry "{title}" {{\n  set gfxpayload={gfx}\n  echo "NEOS: {note}"\n  echo "loading the kernel..."\n  multiboot /boot/matrix32.elf\n  echo "loading initrd.npk (28 MB)..."\n  module /boot/initrd.npk\n'
    for m in mods: s += f'  echo "loading {m}..."\n  module /boot/{m}\n'
    return s + '  echo "starting NEOS..."\n}\n'
cfg = 'set timeout=10\nset default=1\n'
cfg += entry('NEOS', '1024x768x32', cache, 'graphical desktop')
cfg += entry('NEOS - safe mode (text console)', 'text', cache, 'safe mode, all files')
cfg += entry('NEOS - lite (text console, fewer files: faster from USB)', 'text', [], 'lite, without the offline package cache')
sys.stdout.write(cfg)
