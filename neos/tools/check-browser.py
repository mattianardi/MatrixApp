#!/usr/bin/env python3
"""Browser test: neos:// apps with a real form edit (mouse + keyboard), file pages, and an http:// page served by the host."""
import sys, os, threading, http.server, socketserver, tempfile, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screenshot import Session
fails = []
def check(ok, msg):
    if not ok: fails.append(msg); print("FAIL:", msg)
def pix(img, x, y):
    w, h, d = img; o = (y * w + x) * 3; return d[o] << 16 | d[o+1] << 8 | d[o+2]

root = tempfile.mkdtemp(); hits = []
open(os.path.join(root, 'index.html'), 'w').write('<html><head><title>Host page</title></head><body><h1>Hello from the host</h1><p>served over plain HTTP</p><a href="/two.html">next</a></body></html>')
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=root, **k)
    def log_message(self, f, *a): hits.append(a[0] if a else f)
class S(socketserver.TCPServer): allow_reuse_address = True
srv = S(('0.0.0.0', 8099), H); threading.Thread(target=srv.serve_forever, daemon=True).start()

s = Session(boot=14)
pos = [512, 384]
def goto(x, y): s.move(x - pos[0], y - pos[1]); pos[0], pos[1] = x, y
def click(x, y): goto(x, y); s.button(1); s.button(0)
def key(k): s.hmp('sendkey ' + k)

s.cmd('browse neos://settings?m=ui', wait=3)
img = s.shot('/tmp/neos-br-0')
check(pix(img, 700, 90) == 0x0B0F16, 'address bar drawn')
check(pix(img, 140, 187) != 0x0E131C, 'page content drawn')
# edit ui.pin_rows through the web form: click the field, backspace, type 6, press Set
click(230, 335); key('backspace'); key('6'); click(327, 335)
s.cmd('tune get ui.pin_rows', wait=2)
# navigate with the address bar and the keyboard
click(400, 84)
for k in 'neos://about':
    key({':': 'shift-semicolon', '/': 'slash'}.get(k, k))
key('ret')
s.cmd('browse neos://system', wait=2)
s.shot('/tmp/neos-br-1')
s.cmd('browse http://10.0.2.2:8099/index.html', wait=6)
img = s.shot('/tmp/neos-br-2')
check(any('index.html' in h for h in hits), 'the browser fetched the page over HTTP (server saw GET /index.html)')
check(pix(img, 120, 125) in (0xFFD479,) or pix(img, 130, 128) != 0x0E131C, 'http page rendered')
s.cmd('browse https://example.com/', wait=2)
s.cmd('browse neos://www/demo.html', wait=2)
img = s.shot('/tmp/neos-br-3')
out = s.close()
check('ui.pin_rows = 6' in out or re.search(r'ui\.pin_rows = 6', out), 'form edit through the browser changed the setting (tune get shows 6)')
print("BROWSER TEST OK" if not fails else "BROWSER TEST FAILED")
sys.exit(1 if fails else 0)
