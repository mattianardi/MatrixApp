#!/usr/bin/env python3
"""Chromium (the real engine, headless) running ON NEOS: it loads a page, runs its JavaScript and prints the resulting DOM.
Needs build/chrome.npk (tools/mkchrome.py) and the Python bundle's glibc. Boots with 2 GiB of RAM."""
import os, re, sys, subprocess, tempfile
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'); os.chdir(root)
build = 'build'
need = [f'{build}/pycache/glibc.npk', f'{build}/chrome.npk']
if not all(os.path.exists(f) for f in need): print("CHROME TEST SKIPPED (no build/chrome.npk: tools/mkchrome.py)"); sys.exit(0)
sys.path.insert(0, 'tools')
from screenshot import Session
page = '<html><body><h1>hello from neos</h1><script>document.body.append(" js "+(6*7)); document.title = "t" + [1,2,3].map(x => x*x).join("")</script></body></html>'
ses = Session(boot=60, mem='2048', initrd=','.join([f'{build}/initrd.npk', f'{build}/pycache/glibc.npk', f'{build}/chrome.npk']))
ses.run('pkg install glibc', timeout=600)
ses.run('write /tmp/t.html ' + page, timeout=60)
cmd = os.environ.get('CHROME_CMD', 'run /bin/busybox env LD_LIBRARY_PATH=/opt/chrome/lib /opt/chrome/headless_shell.real --no-sandbox --disable-gpu --single-process --no-zygote --font-render-hinting=none --dump-dom file:///tmp/t.html')
out = ses.run(cmd, timeout=int(os.environ.get('CHROME_TIMEOUT', '1500')))
tail = ses.run('mem', timeout=60)
log = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', ses.close())
ok = 'js 42' in out
print(re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out)[-3000:]); print(re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', tail)[-300:])
print("CHROME TEST OK" if ok else "CHROME TEST FAILED"); sys.exit(0 if ok else 1)
