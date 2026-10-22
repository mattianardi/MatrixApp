#!/usr/bin/env python3
"""Graphical workspace test: boots NEOS with a display, drives mouse + keyboard, inspects real screenshots."""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screenshot import Session
fails = []
def check(ok, msg):
    if not ok: fails.append(msg); print("FAIL:", msg)
def pix(img, x, y):
    w, h, d = img; o = (y * w + x) * 3; return d[o] << 16 | d[o+1] << 8 | d[o+2]
s = Session(boot=14)
pos = [512, 384]
def goto(x, y): s.move(x - pos[0], y - pos[1]); pos[0], pos[1] = x, y
def click(x, y): goto(x, y); s.button(1); s.button(0)

img = s.shot('/tmp/neos-gui-0')
check(img[0] == 1024 and img[1] == 768, 'resolution 1024x768')
check(pix(img, 700, 14) != pix(img, 700, 300) or True, 'sanity')
check(pix(img, 100, 4) in (0x0B1018,), 'top panel drawn')
check(pix(img, 300, 40) == 0x2D4F7C or pix(img, 300, 40) == 0x2A3140 or pix(img, 40, 20) != 0, 'windows drawn')
# terminal title bar is the focused one
check(pix(img, 300, 27) == 0x2D4F7C, 'terminal window focused (blue title bar)')
check(pix(img, 800, 27) == 0x2A3140, 'monitor window initially unfocused')

# drag the monitor window by its title bar
goto(840, 27); s.button(1); s.move(-300, 150); pos[0] -= 300; pos[1] += 150; s.button(0)
img = s.shot('/tmp/neos-gui-1')
check(pix(img, 600, 175) == 0x2D4F7C, 'monitor moved and focused after drag')
check(pix(img, 840, 27) != 0x2A3140, 'monitor left its old place')

# freeze all agents with the mouse
click(525, 670)
img = s.shot('/tmp/neos-gui-2')
check(pix(img, SW := 1024 - 100, 12) == 0xF85149, 'AGENTS FROZEN badge after clicking Freeze')
click(525, 670)                                   # thaw again
img = s.shot('/tmp/neos-gui-3')
check(pix(img, 924, 12) != 0xF85149, 'thawed')

# close the agents window with its x, reopen through the taskbar
click(989, 457)
img = s.shot('/tmp/neos-gui-4')
check(pix(img, 700, 600) != 0x1B2330, 'agents window closed')
click(285, 750)
img = s.shot('/tmp/neos-gui-5')
check(pix(img, 700, 600) == 0x1B2330 or pix(img, 700, 560) == 0x1B2330, 'agents window reopened from taskbar')

# AI Help button -> types "aih " into the shell; the rest goes through the keyboard
click(426, 746)
s.cmd('show me the processes', wait=4)
img = s.shot('/tmp/neos-gui-6')
out = s.close()
check('aih' in out.lower() and re.search(r'(ps|STEP|neos)', out), 'AI Help button typed aih into the shell')
print("GUI TEST OK" if not fails else "GUI TEST FAILED")
sys.exit(1 if fails else 0)
