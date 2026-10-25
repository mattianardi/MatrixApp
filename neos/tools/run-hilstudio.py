#!/usr/bin/env python3
"""Run HIL Studio ON NEOS in a virtual machine and open it in your browser (Windows, Linux or macOS with Python 3.10+ and QEMU).

  python run-hilstudio.py --hilstudio C:\\Users\\you\\Documents\\VERA --iso neos.iso [--qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe"]

What it does: downloads the Linux wheels HIL Studio needs (pip download), serves them and the program to the guest on a local
port, starts NEOS from the ISO in QEMU (a window shows the NEOS desktop), types the install and start commands into the NEOS
shell through a serial socket, waits until the server answers and opens http://127.0.0.1:8800 in your browser.
Nothing in the HIL Studio folder is modified. Ctrl+C powers NEOS off."""
import argparse, re, functools, http.server, io, os, shutil, socket, socketserver, subprocess, sys, tarfile, tempfile, threading, time, urllib.request, webbrowser

PKGS = 'fastapi uvicorn websockets python-multipart fmpy bcrypt pydantic nptdms pyelftools psutil PyYAML pyserial'.split()
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--hilstudio', required=True, help='folder that contains the "hilstudio" Python package (the project root)')
ap.add_argument('--iso', default='neos.iso')
ap.add_argument('--qemu', default='qemu-system-x86_64')
ap.add_argument('--port', type=int, default=8800, help='port on this computer for HIL Studio')
ap.add_argument('--http-port', type=int, default=8099, help='temporary port that serves the wheels and the program to NEOS')
ap.add_argument('--serial-port', type=int, default=4555)
ap.add_argument('--mem', default='1024')
ap.add_argument('--accel', default='', help='e.g. whpx (Windows Hypervisor Platform), kvm, hvf: much faster than the default software emulation')
ap.add_argument('--headless', action='store_true', help='no QEMU window')
ap.add_argument('--no-browser', action='store_true')
a = ap.parse_args()

pkg_dir = os.path.join(a.hilstudio, 'hilstudio')
if not os.path.isdir(pkg_dir): sys.exit(f'{a.hilstudio}: no "hilstudio" folder inside')
if not os.path.exists(a.iso): sys.exit(f'{a.iso}: not found')
qemu = shutil.which(a.qemu) or (a.qemu if os.path.exists(a.qemu) else None)
if not qemu:
    for cand in (r'C:\Program Files\qemu\qemu-system-x86_64.exe', r'C:\Program Files (x86)\qemu\qemu-system-x86_64.exe', '/usr/bin/qemu-system-x86_64', '/opt/homebrew/bin/qemu-system-x86_64'):
        if os.path.exists(cand): qemu = cand; break
if not qemu: sys.exit('QEMU not found: install it or pass --qemu "<full path>\\qemu-system-x86_64.exe"')

www = tempfile.mkdtemp(prefix='neos-hil-')
print('[1/5] downloading the Linux wheels (pip download)...', flush=True)
subprocess.run([sys.executable, '-m', 'pip', 'download', '--only-binary=:all:', '--platform', 'manylinux2014_x86_64', '--platform', 'manylinux_2_28_x86_64', '--platform', 'manylinux_2_17_x86_64',
                '--python-version', '3.13', '--implementation', 'cp', '--abi', 'cp313', '--abi', 'abi3', '--abi', 'none', '-d', www] + PKGS, check=True, stdout=subprocess.DEVNULL)
for f in os.listdir(www):
    if f.startswith('cmake'): os.remove(os.path.join(www, f))
names = sorted(f.split('-')[0] for f in os.listdir(www) if f.endswith('.whl'))
open(os.path.join(www, 'req.txt'), 'w').write('\n'.join(names) + '\n')
print('[2/5] packing the program (only read from your folder)...', flush=True)
def skip(ti):
    return None if any(p in ti.name for p in ('__pycache__', '.pyc')) else ti
with tarfile.open(os.path.join(www, 'hilstudio.tar.gz'), 'w:gz') as t: t.add(pkg_dir, arcname='hilstudio', filter=skip)

class Q(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *x): pass
class S(socketserver.ThreadingTCPServer): allow_reuse_address = True; daemon_threads = True
srv = S(('0.0.0.0', a.http_port), functools.partial(Q, directory=www)); threading.Thread(target=srv.serve_forever, daemon=True).start()

print('[3/5] starting NEOS in QEMU...', flush=True)
cmd = [qemu, '-cdrom', a.iso, '-boot', 'd', '-m', a.mem, '-cpu', 'max', '-vga', 'std', '-nic', f'user,model=e1000,hostfwd=tcp::{a.port}-:8800',
       '-serial', f'tcp:127.0.0.1:{a.serial_port},server=on,wait=off']
if a.accel: cmd += ['-accel', a.accel]
if a.headless: cmd += ['-display', 'none']
qp = subprocess.Popen(cmd)
sock = None
for _ in range(60):
    try: sock = socket.create_connection(('127.0.0.1', a.serial_port), timeout=2); break
    except OSError: time.sleep(1)
if not sock: qp.terminate(); sys.exit('cannot reach the QEMU serial port')
sock.settimeout(1)
buf = ''
def read_until(prompt='neos> ', timeout=3600, echo=True):
    global buf
    t0 = time.time(); start = len(buf)
    while time.time() - t0 < timeout:
        try:
            d = sock.recv(4096).decode('latin1')
            if not d: break
            if echo: sys.stdout.write(d.replace('\r', '')); sys.stdout.flush()
            buf += d
        except socket.timeout: pass
        if re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', buf[start:]).rstrip().endswith(prompt.strip()): return True
        if qp.poll() is not None: return False
    return False
def run(line, timeout=3600):
    sock.sendall(line.encode() + b'\n'); return read_until(timeout=timeout)

try:
    print('[4/5] waiting for the NEOS shell (about a minute in software emulation)...', flush=True)
    if not read_until(timeout=600): sys.exit('NEOS did not reach its prompt')
    h = f'http://10.0.2.2:{a.http_port}'
    run(f'pkg install pip:--trusted-host 10.0.2.2 --no-index --no-deps --find-links {h}/ -r {h}/req.txt')
    run(f'run /bin/busybox sh -c "mkdir -p /opt/hil && cd /opt/hil && wget -q -O - {h}/hilstudio.tar.gz | tar xz"')
    run('run /bin/busybox sh -c "cd /opt/hil && /opt/python3/bin/python3 -m hilstudio.server --host 0.0.0.0 --port 8800 > /tmp/hil.log 2>&1 &"', timeout=300)
    print(f'\n[5/5] waiting for HIL Studio to answer on http://127.0.0.1:{a.port} ...', flush=True)
    for _ in range(300):
        try:
            if b'ok' in urllib.request.urlopen(f'http://127.0.0.1:{a.port}/api/health', timeout=5).read(): break
        except Exception: time.sleep(2)
    else: sys.exit('HIL Studio did not answer: see the NEOS window / the log above')
    url = f'http://127.0.0.1:{a.port}/'
    print(f'\nHIL Studio is running ON NEOS: {url}  (first login master / master). Ctrl+C to stop.', flush=True)
    if not a.no_browser: webbrowser.open(url)
    while qp.poll() is None: time.sleep(2)
except KeyboardInterrupt:
    print('\npowering NEOS off...')
    try: sock.sendall(b'poweroff\n'); time.sleep(2)
    except Exception: pass
finally:
    if qp.poll() is None: qp.terminate()
    shutil.rmtree(www, ignore_errors=True)
