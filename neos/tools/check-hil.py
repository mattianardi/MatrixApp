#!/usr/bin/env python3
"""A Python test-bench / HiL style application (threads, sockets, sqlite, asyncio, ctypes, fork, RT scheduling...) must run on NEOS:
the probe script linuxtests/hil_smoke.py is installed from the offline cache and executed by the NEOS Python."""
import os, re, sys, subprocess, shutil, tempfile
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'); os.chdir(root)
build = 'build'
need = [f'{build}/pycache/glibc.npk', f'{build}/pycache/python3.npk']
if not all(os.path.exists(f) for f in need): print("HIL TEST SKIPPED (no python bundle)"); sys.exit(0)
w = tempfile.mkdtemp(); open(f'{w}/empty.npk', 'wb').write(b'NPK1\nEND\n')
open(f'{w}/info', 'w').write('name=hilcheck\nversion=1\ndescription=HiL application probes\ndepends=python3\n')
subprocess.run([sys.executable, 'tools/npk_add.py', f'{w}/empty.npk', f'{w}/hilcheck.npk', f'.pkginfo={w}/info', 'smoke.py=' + (sys.argv[1] if len(sys.argv) > 1 else 'linuxtests/hil_smoke.py')], check=True)
sys.path.insert(0, 'tools')
from screenshot import Session
ses = Session(boot=30, initrd=','.join([f'{build}/initrd.npk', f'{build}/pycache/glibc.npk', f'{build}/pycache/ca-certificates.npk', f'{build}/pycache/python3.npk', f'{w}/hilcheck.npk']))
ses.run('pkg install hilcheck', timeout=600)
ses.run('python3 /opt/hilcheck/smoke.py', timeout=int(os.environ.get('HIL_TIMEOUT', '400')))
out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', ses.close())
if 'HIL SMOKE OK' not in out: print(out[-3500:])
else: print('\n'.join(l for l in out.splitlines() if l.startswith('HIL ')))
print("HIL TEST OK" if 'HIL SMOKE OK' in out else "HIL TEST FAILED"); sys.exit(0 if 'HIL SMOKE OK' in out else 1)
