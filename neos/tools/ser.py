#!/usr/bin/env python3
"""Type commands into a NEOS started by tools/neos-vm.sh (console on a TCP port) and print the output: tools/ser.py 'cmd' '@30:slow cmd' ..."""
import socket, time, re, sys
s=socket.create_connection(('127.0.0.1',int(__import__('os').environ.get('NEOS_SER', '4561'))),timeout=5); s.settimeout(2)
def rd():
    d=b''
    try:
        while True:
            x=s.recv(4096)
            if not x: break
            d+=x
    except Exception: pass
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',d.decode('latin1'))
rd()
for c in sys.argv[1:]:
    w = 4
    if c.startswith('@'): w, c = int(c[1:c.index(':')]), c[c.index(':')+1:]
    s.sendall(c.encode()+b'\n'); time.sleep(w); print(rd().replace('\r',''), flush=True)
