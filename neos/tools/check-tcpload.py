#!/usr/bin/env python3
"""Parallel HTTP clients from the host against an asyncio server inside NEOS (port-forwarded): every request must be answered."""
import os, re, sys, subprocess, tempfile, threading, time, urllib.request, concurrent.futures
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'); os.chdir(root)
build = 'build'
if not all(os.path.exists(f'{build}/pycache/{n}.npk') for n in ('glibc', 'python3')): print("TCPLOAD TEST SKIPPED (no python bundle)"); sys.exit(0)
w = tempfile.mkdtemp(); open(f'{w}/empty.npk', 'wb').write(b'NPK1\nEND\n'); open(f'{w}/info', 'w').write('name=loadsrv\nversion=1\ndescription=tcp load\ndepends=python3\n')
subprocess.run([sys.executable, 'tools/npk_add.py', f'{w}/empty.npk', f'{w}/loadsrv.npk', f'.pkginfo={w}/info', 'server.py=linuxtests/loadserver.py'], check=True)
sys.path.insert(0, 'tools')
from screenshot import Session
ses = Session(boot=30, nic='user,model=e1000,hostfwd=tcp::18801-:8800', initrd=','.join([f'{build}/initrd.npk', f'{build}/pycache/glibc.npk', f'{build}/pycache/ca-certificates.npk', f'{build}/pycache/python3.npk', f'{w}/loadsrv.npk']))
ses.run('pkg install loadsrv', timeout=900)
ses.run('run /bin/busybox sh -c "/opt/python3/bin/python3 /opt/loadsrv/server.py > /tmp/load.log 2>&1 &"', timeout=60)
ok = False
for _ in range(120):
    try: urllib.request.urlopen('http://127.0.0.1:18801/', timeout=5).read(); ok = True; break
    except Exception: time.sleep(2)
def one(i):
    t = time.time()
    try: n = len(urllib.request.urlopen('http://127.0.0.1:18801/', timeout=40).read()); return (n == 60000, time.time() - t)
    except Exception as e: return (False, time.time() - t)
ses.run('tune set net.tcp_trace 1', timeout=30)
ka = subprocess.run(['curl', '-s', '-m', '20', '-o', '/dev/null', '-o', '/dev/null', '-w', '%{http_code}:%{size_download}:%{time_total} ', 'http://127.0.0.1:18801/a', 'http://127.0.0.1:18801/b'], capture_output=True, text=True).stdout
print('keep-alive (two requests on one connection):', ka)
ses.run('tune set net.tcp_trace 0', timeout=30)
res = []
for par in (1, 4, 8, 16):
    with concurrent.futures.ThreadPoolExecutor(par) as ex: r = list(ex.map(one, range(max(24, par * 3))))
    res.append((par, sum(1 for a, _ in r if a), len(r), max(t for _, t in r)))
    print(f'parallel {par}: {res[-1][1]}/{res[-1][2]} ok, slowest {res[-1][3]:.1f}s', flush=True)
time.sleep(8); tcp = ses.run('tcp', timeout=60); log = ses.run('run /bin/busybox tail -n 15 /tmp/load.log', timeout=60)
out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', ses.close())
left = len(re.findall(r'established', re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', tcp)))
print('connections left open after the load:', left)
good = ok and all(a == b for _, a, b, _ in res) and left <= 4      # the loopback pair of the interpreter itself, a few in flight
print(re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', tcp)[-1500:]); print(re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', log)[-800:])
print("TCPLOAD TEST OK" if good else "TCPLOAD TEST FAILED"); sys.exit(0 if good else 1)
