#!/usr/bin/env python3
"""Serial ports: the 16550 UART on COM3 (/dev/ttyS2) and a USB serial adapter (QEMU's FTDI usb-serial -> /dev/ttyUSB0),
used by a termios/select program exactly like pyserial would, with the other end on host TCP sockets."""
import os, re, sys, socket, subprocess, tempfile, threading, time
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'); os.chdir(root)
build = 'build'
need = [f'{build}/pycache/glibc.npk', f'{build}/pycache/python3.npk']
if not all(os.path.exists(f) for f in need): print("SERIAL TEST SKIPPED (no python bundle)"); sys.exit(0)
w = tempfile.mkdtemp(); open(f'{w}/empty.npk', 'wb').write(b'NPK1\nEND\n')
open(f'{w}/info', 'w').write('name=sercheck\nversion=1\ndescription=serial probe\ndepends=python3\n')
subprocess.run([sys.executable, 'tools/npk_add.py', f'{w}/empty.npk', f'{w}/sercheck.npk', f'.pkginfo={w}/info', 'echo.py=linuxtests/serial_echo.py'], check=True)
P1, P2 = 45611, 45612
sys.path.insert(0, 'tools')
from screenshot import Session
extra = ['-chardev', f'socket,id=c1,host=127.0.0.1,port={P1},server=on,wait=off', '-serial', 'null', '-serial', 'chardev:c1',
         '-device', 'qemu-xhci', '-chardev', f'socket,id=c2,host=127.0.0.1,port={P2},server=on,wait=off', '-device', 'usb-serial,chardev=c2']
ses = Session(boot=30, initrd=','.join([f'{build}/initrd.npk', f'{build}/pycache/glibc.npk', f'{build}/pycache/ca-certificates.npk', f'{build}/pycache/python3.npk', f'{w}/sercheck.npk']), extra=extra)
got = {}; ready = threading.Event(); stop = threading.Event()
def talk(name, port, msg):
    s = None
    for _ in range(200):
        try: s = socket.create_connection(('127.0.0.1', port), timeout=2); break
        except OSError: time.sleep(0.3)
    if not s: got[name] = b'(no connection)'; return
    s.settimeout(1); data = b''; sent = False
    while not stop.is_set() and b'\n' not in data:
        if not sent and ready.is_set(): s.sendall(msg); sent = True                # the program has opened the ports by now
        try: data += s.recv(256)
        except OSError: pass
    got[name] = data
ths = [threading.Thread(target=talk, args=('com3', P1, b'hello com3\n')), threading.Thread(target=talk, args=('usb', P2, b'hello usb\n'))]
[t.start() for t in ths]
ses.run('pkg install sercheck', timeout=900)
ses.cmd('python3 /opt/sercheck/echo.py', wait=1)
if ses.expect('SER ready', timeout=180): ready.set()
ses.expect('SER done', timeout=180)
time.sleep(2); stop.set()
[t.join(15) for t in ths]
out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', ses.close())
fails = []; print('host side:', got)
def check(cond, what):
    if not cond: fails.append(what); print('MISSING:', what)
check('/dev/ttyUSB0' in out, 'USB serial adapter registered as /dev/ttyUSB0')
check("SER ready ['/dev/ttyS2', '/dev/ttyUSB0']" in out, 'both ports opened by the program')
check(b'ECHO HELLO COM3' in got.get('com3', b''), f"COM3 answer ({got.get('com3')!r})")
check(b'ECHO HELLO USB' in got.get('usb', b''), f"USB answer ({got.get('usb')!r})")
check('SER done 2 of 2' in out, 'program finished')
if fails: print(out[-2500:])
print("SERIAL TEST OK" if not fails else "SERIAL TEST FAILED"); sys.exit(1 if fails else 0)
