#!/usr/bin/env python3
"""Network for unmodified Linux programs: sockets (TCP, UDP, loopback), resolver files, busybox wget, a Go net/http server+client,
and the NEOS browser showing a page served by a Go program running inside NEOS."""
import sys, os, threading, http.server, socketserver, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screenshot import Session
fails = []
def check(ok, msg):
    if not ok: fails.append(msg); print("FAIL:", msg)
def pix(img, x, y):
    w, h, d = img; o = (y * w + x) * 3; return d[o] << 16 | d[o+1] << 8 | d[o+2]
root = tempfile.mkdtemp(); open(os.path.join(root, 'index.html'), 'w').write('<html>host-page</html>')
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=root, **k)
    def log_message(self, *a): pass
class S(socketserver.TCPServer): allow_reuse_address = True
srv = S(('0.0.0.0', 8099), H); threading.Thread(target=srv.serve_forever, daemon=True).start()

s = Session(boot=14)
s.cmd('run /bin/netcheck 10.0.2.2 8099', wait=8)
s.cmd('run /bin/busybox wget -q -O - http://10.0.2.2:8099/index.html', wait=5)
s.cmd('run /bin/webserver', wait=10)
s.cmd('run -b /bin/webserver serve', wait=2)
check(s.expect('serving on http://127.0.0.1:8081/', 60), 'Go server started in the background')
s.cmd('browse http://127.0.0.1:8081/', wait=6)
img = s.shot('/tmp/neos-net-1')
check(any(pix(img, x, 128) == 0xFFD479 for x in range(78, 400)), 'browser shows the Go page heading (served by a Go program inside NEOS)')
s.cmd('browse http://127.0.0.1:8081/echo?q=from+the+browser', wait=5)
img = s.shot('/tmp/neos-net-2')
check(any(pix(img, x, y) == 0xA5D6FF for x in range(78, 200) for y in range(112, 132)), 'form-style request to the Go server returned the echo page')
out = s.close()
for want in ['NET tcp loopback: 19 bytes: HELLO OVER LOOPBACK', 'NET udp loopback: poll=1 8 bytes: datagram', 'NET getaddrinfo localhost -> 127.0.0.1',
             'NET host http: HTTP/1.0 200 OK', 'host-page', 'GO http client got 200, 287 bytes, contains title: true', 'serving on http://127.0.0.1:8081/']:
    check(want in out, 'missing: ' + want)
if fails: print(out[-1800:])
print("NETAPPS TEST OK" if not fails else "NETAPPS TEST FAILED")
sys.exit(1 if fails else 0)
