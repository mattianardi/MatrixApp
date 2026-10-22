#!/usr/bin/env python3
"""Boot NEOS with a display and save screenshots of the main screens into docs/screens/."""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screenshot import Session
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'screens'); os.makedirs(out, exist_ok=True)
def shot(s, name): s.shot(os.path.join(out, name)); print('saved', name)
s = Session(boot=16)
pos = [512, 384]
def goto(x, y): s.move(x - pos[0], y - pos[1]); pos[0], pos[1] = x, y
def click(x, y): goto(x, y); s.button(1); s.button(0)
shot(s, '01-desktop')
s.cmd('ps | where prio<6 | sort ticks desc | cols name prio ticks | pin Threads', wait=3)
s.cmd('aih mostrami i processi che usano piu CPU', wait=4)
shot(s, '02-aih-and-pinned-view')
s.cmd('n', wait=1)
s.cmd('run /bin/wintest.exe one two', wait=8)
s.cmd('run /bin/busybox sh -c "ls /mnt/c/shared; cat /mnt/c/shared/win.txt"', wait=4)
shot(s, '03-windows-and-linux-programs')
s.cmd('browse neos://home', wait=3); shot(s, '04-browser-home')
s.cmd('browse neos://settings?m=sched', wait=3); shot(s, '05-browser-settings')
s.cmd('browse neos://system', wait=3); shot(s, '06-browser-system')
s.cmd('browse neos://www/demo.html', wait=3); shot(s, '07-browser-webapp')
s.cmd('run -b /bin/webserver serve', wait=2)
s.expect('serving on http://127.0.0.1:8081/', 90)
s.cmd('browse http://127.0.0.1:8081/', wait=6); shot(s, '08-go-web-server-in-browser')
s.close()
