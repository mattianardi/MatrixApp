#!/usr/bin/env python3
"""Package ecosystem test: tar, deb, pip (wheel), apt-lite (Debian-style repository) and the offline cache (python3 + glibc),
all installed from inside NEOS through `pkg install <backend>:<spec>`, all showing up in `pkg installed`."""
import os, re, sys, subprocess, tempfile, threading, http.server, socketserver, zipfile, hashlib, base64, shutil, gzip
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'); os.chdir(root)
w = tempfile.mkdtemp(); www = os.path.join(w, 'www'); os.makedirs(www)
build = 'build'
need = [f'{build}/pycache/glibc.npk', f'{build}/pycache/python3.npk']
if not all(os.path.exists(f) for f in need) or not shutil.which('dpkg-deb'):
    print("PKG TEST SKIPPED (no python bundle or dpkg-deb on this host)"); sys.exit(0)

# fixtures -------------------------------------------------------------
def sh(*a, **k): subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **k)
d = os.path.join(w, 'deb'); os.makedirs(f'{d}/DEBIAN'); os.makedirs(f'{d}/usr/bin')
shutil.copy(f'{build}/linux/hello_musl', f'{d}/usr/bin/hello-musl')
open(f'{d}/DEBIAN/control', 'w').write('Package: hellodeb\nVersion: 1.2-1\nArchitecture: amd64\nMaintainer: t <t@e.org>\nDescription: test\n')
sh('dpkg-deb', '-Zgzip', '--build', d, f'{www}/hellodeb_1.2-1_amd64.deb')
t = os.path.join(w, 'tar', 'mytool-2.5', 'bin'); os.makedirs(t); shutil.copy(f'{build}/linux/hello_glibc', f'{t}/mytool')
sh('tar', 'czf', f'{www}/mytool-2.5.tar.gz', '-C', os.path.join(w, 'tar'), 'mytool-2.5')
files = {'neosdemo/__init__.py': b'def hello():\n    return "hello from a wheel installed by pip on NEOS"\n\ndef hello_cli():\n    print(hello())\n',
         'neosdemo-0.1.dist-info/METADATA': b'Metadata-Version: 2.1\nName: neosdemo\nVersion: 0.1\nSummary: demo\n',
         'neosdemo-0.1.dist-info/WHEEL': b'Wheel-Version: 1.0\nGenerator: hand\nRoot-Is-Purelib: true\nTag: py3-none-any\n',
         'neosdemo-0.1.dist-info/entry_points.txt': b'[console_scripts]\nneosdemo = neosdemo:hello_cli\n'}
rec = []
with zipfile.ZipFile(f'{www}/neosdemo-0.1-py3-none-any.whl', 'w') as z:
    for n, b in files.items(): z.writestr(n, b); rec.append(f"{n},sha256={base64.urlsafe_b64encode(hashlib.sha256(b).digest()).rstrip(b'=').decode()},{len(b)}")
    z.writestr('neosdemo-0.1.dist-info/RECORD', '\n'.join(rec + ['neosdemo-0.1.dist-info/RECORD,,']) + '\n')
A = os.path.join(w, 'apt'); os.makedirs(f'{A}/aptlib/DEBIAN'); os.makedirs(f'{A}/aptlib/usr/share/aptlib'); os.makedirs(f'{A}/aptdemo/DEBIAN'); os.makedirs(f'{A}/aptdemo/usr/bin')
open(f'{A}/aptlib/usr/share/aptlib/msg', 'w').write('message from the library package\n')
open(f'{A}/aptlib/DEBIAN/control', 'w').write('Package: aptlib\nVersion: 1.0-1\nArchitecture: amd64\nMaintainer: t <t@e.org>\nDescription: demo library\n')
open(f'{A}/aptdemo/usr/bin/aptdemo', 'w').write('#!/bin/sh\necho "aptdemo says: $(cat /opt/apt/usr/share/aptlib/msg)"\n'); os.chmod(f'{A}/aptdemo/usr/bin/aptdemo', 0o755)
open(f'{A}/aptdemo/DEBIAN/control', 'w').write('Package: aptdemo\nVersion: 2.0-1\nArchitecture: amd64\nMaintainer: t <t@e.org>\nDepends: aptlib, libc6\nDescription: demo program\n')
R = f'{www}/aptrepo'; os.makedirs(f'{R}/pool'); os.makedirs(f'{R}/dists/test/main/binary-amd64')
sh('dpkg-deb', '-Zgzip', '--build', f'{A}/aptlib', f'{R}/pool/aptlib_1.0-1_amd64.deb'); sh('dpkg-deb', '-Zxz', '--build', f'{A}/aptdemo', f'{R}/pool/aptdemo_2.0-1_amd64.deb')
pk = subprocess.run(['dpkg-scanpackages', 'pool', '/dev/null'], cwd=R, capture_output=True).stdout
open(f'{R}/dists/test/main/binary-amd64/Packages', 'wb').write(pk); open(f'{R}/dists/test/main/binary-amd64/Packages.gz', 'wb').write(gzip.compress(pk))

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=www, **k)
    def log_message(self, *a): pass
class S(socketserver.TCPServer): allow_reuse_address = True
srv = S(('0.0.0.0', 8099), H); threading.Thread(target=srv.serve_forever, daemon=True).start()

cmds = ['pkg install tar:http://10.0.2.2:8099/mytool-2.5.tar.gz', 'mytool x',
        'pkg install deb:http://10.0.2.2:8099/hellodeb_1.2-1_amd64.deb', 'hello-musl',
        'pkg install pip:http://10.0.2.2:8099/neosdemo-0.1-py3-none-any.whl', 'neosdemo',
        'write /etc/neos/apt.conf mirror=http://10.0.2.2:8099/aptrepo dist=test components=main', 'pkg install apt:update', 'pkg install apt:aptdemo', 'aptdemo',
        'pkg installed', 'pkg remove apt:aptdemo', 'pkg remove tar:mytool', 'pkg installed']
sys.path.insert(0, 'tools')
from screenshot import Session
ses = Session(boot=30, initrd=','.join([f'{build}/initrd.npk', f'{build}/pycache/glibc.npk', f'{build}/pycache/ca-certificates.npk', f'{build}/pycache/python3.npk']))
for c in cmds: ses.run(c, timeout=600)
out = ses.clean() if False else None
out = ses.close(); out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out)
fails = []
def check(s):
    if s not in out: fails.append(s); print('MISSING:', s)
for s in ['pkg: installed mytool 2.5 into /opt/mytool', 'hello from an unmodified Linux binary (glibc, static)',
          'pkg: installed hellodeb 1.2-1 into /opt/hellodeb', 'hello from a musl-libc static binary',
          'installed python3 3.13', 'Successfully installed neosdemo', 'hello from a wheel installed by pip on NEOS',
          'pkg: will install: aptlib=1.0-1 aptdemo=2.0-1', 'aptdemo says: message from the library package',
          'pip:neosdemo', '2.0-1    apt', '1.2-1    deb', '2.5      tar', 'pkg: removed aptdemo', 'pkg: removed mytool']:
    check(s)
if fails: print(out[-3000:])
print("PKG TEST OK" if not fails else "PKG TEST FAILED"); sys.exit(1 if fails else 0)
