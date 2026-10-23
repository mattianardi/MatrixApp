#!/usr/bin/env python3
"""Linux graphics/input devices: a Python program paints /dev/fb0 through mmap (the screen is read back from QEMU and checked) and receives
a key press and pointer movement through /dev/input/event0 and event1, the way Qt linuxfb / SDL would."""
import os, re, sys, subprocess, tempfile, time
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'); os.chdir(root)
build = 'build'
need = [f'{build}/pycache/glibc.npk', f'{build}/pycache/python3.npk']
if not all(os.path.exists(f) for f in need): print("FB TEST SKIPPED (no python bundle)"); sys.exit(0)
w = tempfile.mkdtemp(); open(f'{w}/empty.npk', 'wb').write(b'NPK1\nEND\n')
open(f'{w}/info', 'w').write('name=fbcheck\nversion=1\ndescription=fbdev probe\ndepends=python3\n')
subprocess.run([sys.executable, 'tools/npk_add.py', f'{w}/empty.npk', f'{w}/fbcheck.npk', f'.pkginfo={w}/info', 'fbdraw.py=linuxtests/fbdraw.py'], check=True)
sys.path.insert(0, 'tools')
from screenshot import Session
ses = Session(boot=30, initrd=','.join([f'{build}/initrd.npk', f'{build}/pycache/glibc.npk', f'{build}/pycache/ca-certificates.npk', f'{build}/pycache/python3.npk', f'{w}/fbcheck.npk']))
ses.run('pkg install fbcheck', timeout=600)
ses.cmd('python3 /opt/fbcheck/fbdraw.py', wait=1)
ready = ses.expect('FB ready', timeout=120)
time.sleep(1.5)
pix = None
if ready:
    wd, ht, pix = ses.shot(os.path.join(w, 'fb'))
    ses.hmp('sendkey a'); time.sleep(0.5); ses.hmp('mouse_move 12 7'); time.sleep(0.5); ses.hmp('mouse_move -4 3')
ses.expect('FB done', timeout=90)
out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', ses.close())
fails = []
def check(cond, what):
    if not cond: fails.append(what); print('MISSING:', what)
check(re.search(r'FB mode 1024 768 32 line 4096 len 3145728', out) is not None, 'fbdev mode 1024x768x32')
check('FB ready NEOS keyboard' in out, 'evdev device name')
if pix:
    def at(x, y): o = (y * wd + x) * 3; return tuple(pix[o:o + 3])
    check(at(300, 100) == (255, 0, 0), f'top half red, got {at(300, 100)}')
    check(at(300, ht - 100) == (0, 0, 255), f'bottom half blue, got {at(300, ht - 100)}')
else: check(False, 'screen capture')
check('EV key 30' in out, 'key press (KEY_A = 30) through /dev/input/event0')
check('EV rel 0 12' in out or 'EV rel 0' in out, 'pointer movement through /dev/input/event1')
check('FB done' in out, 'program finished')
if fails: print(out[-2500:])
print("FB TEST OK" if not fails else "FB TEST FAILED"); sys.exit(1 if fails else 0)
