#!/usr/bin/env python3
"""HIL Studio (FastAPI + web UI, github.com/mattianardi/hilstudio) running ON NEOS: its Python dependencies are installed with pip from a host
wheelhouse, the program is unpacked, started inside NEOS, and its HTTP API is reached from the host through a forwarded port.
  tools/check-hilstudio.py [path to the hilstudio clone]      (wheelhouse: build/hilwheels, created with pip download on first use)"""
import os, re, sys, subprocess, threading, http.server, socketserver, time, urllib.request, json, functools
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'); os.chdir(root)
src = sys.argv[1] if len(sys.argv) > 1 else '/home/user/hilstudio'
build = 'build'
need = [f'{build}/pycache/glibc.npk', f'{build}/pycache/python3.npk']
if not all(os.path.exists(f) for f in need) or not os.path.isdir(src): print("HILSTUDIO TEST SKIPPED (no python bundle or no clone)"); sys.exit(0)
wh = f'{build}/hilwheels'; os.makedirs(wh, exist_ok=True)
PKGS = 'fastapi uvicorn websockets python-multipart fmpy bcrypt pydantic nptdms pyelftools psutil PyYAML pyserial'
if not any(f.endswith('.whl') for f in os.listdir(wh)):
    subprocess.run([sys.executable, '-m', 'pip', 'download', '--only-binary=:all:', '--platform', 'manylinux2014_x86_64', '--platform', 'manylinux_2_28_x86_64', '--platform', 'manylinux_2_17_x86_64',
                    '--python-version', '3.13', '--implementation', 'cp', '--abi', 'cp313', '--abi', 'abi3', '--abi', 'none', '-d', wh] + PKGS.split(), check=True, stdout=subprocess.DEVNULL)
    for f in os.listdir(wh):
        if f.startswith('cmake'): os.remove(os.path.join(wh, f))                 # fmpy asks for it to build FMUs; not needed to run them
subprocess.run(f'git -C {src} archive --format=tar.gz HEAD hilstudio -o {os.path.abspath(wh)}/hilstudio.tar.gz', shell=True, check=True)
names = sorted(f.split('-')[0] for f in os.listdir(wh) if f.endswith('.whl'))
if os.environ.get('HILSTUDIO_PKGS'): names = os.environ['HILSTUDIO_PKGS'].split(',')                 # debugging: a small subset
open(os.path.join(wh, 'req.txt'), 'w').write('\n'.join(names) + '\n')          # the NEOS shell line is short: the list travels as a file
class S(socketserver.ThreadingTCPServer): allow_reuse_address = True; daemon_threads = True
class Q(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
srv = S(('0.0.0.0', 8099), functools.partial(Q, directory=os.path.abspath(wh))); threading.Thread(target=srv.serve_forever, daemon=True).start()
sys.path.insert(0, 'tools')
from screenshot import Session
ses = Session(boot=30, mem='1024', nic='user,model=e1000,hostfwd=tcp::18800-:8800', initrd=','.join([f'{build}/initrd.npk', f'{build}/pycache/glibc.npk', f'{build}/pycache/ca-certificates.npk', f'{build}/pycache/python3.npk']))
T = int(os.environ.get('HILSTUDIO_STEP_TIMEOUT', '1800'))
log = []
log.append(ses.run('pkg install pip:' + os.environ.get('HILSTUDIO_PIPARGS', '') + ' --trusted-host 10.0.2.2 --no-index --no-deps --find-links http://10.0.2.2:8099/ -r http://10.0.2.2:8099/req.txt', timeout=T))
log.append(ses.run('mem', timeout=60)); log.append(ses.run('run /bin/busybox sh -c "ls -laR /tmp"', timeout=60))
if os.environ.get('HILSTUDIO_PKGS'): out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', ses.close()); print(out[-3000:]); sys.exit(0)
log.append(ses.run('run /bin/busybox sh -c "mkdir -p /opt/hil && cd /opt/hil && wget -q -O - http://10.0.2.2:8099/hilstudio.tar.gz | tar xz && ls /opt/hil"', timeout=T))
log.append(ses.run('run /bin/busybox sh -c "cd /opt/hil && /opt/python3/bin/python3 -m hilstudio.server --host 0.0.0.0 --port 8800 > /tmp/hil.log 2>&1 &"', timeout=120))
health = None; t0 = time.time()
while time.time() - t0 < 600 and health is None:
    try: health = urllib.request.urlopen('http://127.0.0.1:18800/api/health', timeout=5).read().decode()
    except Exception: time.sleep(5)
page = None
try: page = urllib.request.urlopen('http://127.0.0.1:18800/', timeout=30).read(400).decode('latin1')
except Exception as e: page = f'error {e}'
if health and os.environ.get('HILSTUDIO_SHOT'):                                           # the real UI, rendered by a real browser, served by NEOS
    r = subprocess.run(['node', 'tools/shot-web.cjs', 'http://127.0.0.1:18800/', os.environ['HILSTUDIO_SHOT']], capture_output=True, text=True, timeout=300)
    print('browser:', (r.stdout + r.stderr).strip()[-300:])
log.append(ses.run('run /bin/busybox tail -n 22 /tmp/hil.log', timeout=60))
out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', ses.close())
open('build/hilstudio_session.log', 'w').write(out)
print('health:', health); print('page:', (page or '')[:120].replace('\n', ' '))
ok = health is not None and 'ok' in health.lower() or (health or '').strip().startswith('{')
if not ok: print(out[-3500:])
print("HILSTUDIO TEST OK" if ok else "HILSTUDIO TEST FAILED"); sys.exit(0 if ok else 1)
