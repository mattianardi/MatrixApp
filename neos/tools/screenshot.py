#!/usr/bin/env python3
"""Boot NEOS, optionally drive the shell over serial and the mouse through the QEMU monitor, save PNG screenshots.
   tools/screenshot.py out_prefix [--cmd 'shell command']... [--mouse 'dx dy' | --click | --release]... [--shot name]"""
import socket, subprocess, sys, time, os, zlib, struct, tempfile, threading, re
def ppm_to_png(ppm, png):
    d = open(ppm, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); pix = parts[3]
    raw = b''.join(b'\0' + pix[y*w*3:(y+1)*w*3] for y in range(h))
    def ch(t, b): c = struct.pack('>I', len(b)) + t + b; return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(png, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
    return w, h, pix

class Session:
    def __init__(self, boot=14, initrd='build/initrd.npk', extra=None, kargs='', nic='user,model=e1000', mem='512'):
        self.dir = tempfile.mkdtemp()
        self.mon = os.path.join(self.dir, 'mon.sock')
        os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        self.p = subprocess.Popen(['qemu-system-x86_64', '-kernel', 'build/matrix32.elf', '-initrd', initrd, '-m', mem, '-display', 'none',
            '-cpu', 'max', '-serial', 'stdio', '-no-reboot', '-vga', 'std', '-nic', nic, '-monitor', f'unix:{self.mon},server,nowait', '-append', kargs] + (extra or []),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.out = ''
        threading.Thread(target=self._reader, daemon=True).start()
        time.sleep(boot)
        self.lock = threading.Lock()
        self.m = socket.socket(socket.AF_UNIX); self.m.connect(self.mon); time.sleep(0.3); self.m.recv(65536)
        if os.environ.get('NEOS_LIVE'): self.start_live(os.environ['NEOS_LIVE'])         # a PNG of the screen, refreshed while the session runs
    def _reader(self):
        while True:
            b = self.p.stdout.read1(4096) if hasattr(self.p.stdout, 'read1') else self.p.stdout.read(1)
            if not b: break
            self.out += b.decode('latin1')
    def clean(self): return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', self.out)
    def run(self, cmd, timeout=300):
        """Type a command and wait until the shell prints its next prompt (long-running programs included)."""
        mark = len(self.clean()); self.p.stdin.write(cmd.encode() + b'\n'); self.p.stdin.flush()
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(0.5); c = self.clean()[mark:]
            if c.rstrip().endswith('neos>') and len(c) > len(cmd) + 2: return c
        return self.clean()[mark:]
    def expect(self, text, timeout=60):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if text in self.out: return True
            time.sleep(0.3)
        return False
    def hmp(self, c):
        with self.lock:
            self.m.sendall(c.encode() + b'\n'); time.sleep(0.4)
            try: self.m.settimeout(0.5); return self.m.recv(65536)
            except Exception: return b''
    def start_live(self, png, every=4):
        """Keep `png` up to date with the guest's screen (a background thread; the monitor accepts one client, so it shares ours)."""
        def loop():
            ppm = png + '.ppm'
            while self.p.poll() is None:
                try:
                    self.hmp(f'screendump {ppm}'); time.sleep(0.8)
                    if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: ppm_to_png(ppm, png + '.tmp.png'); os.replace(png + '.tmp.png', png)
                except Exception: pass
                time.sleep(every)
        threading.Thread(target=loop, daemon=True).start()
    def cmd(self, c, wait=2.0): self.p.stdin.write(c.encode() + b'\n'); self.p.stdin.flush(); time.sleep(wait)
    def shot(self, name):
        ppm = os.path.join(self.dir, name + '.ppm'); self.hmp(f'screendump {ppm}'); time.sleep(0.8)
        return ppm_to_png(ppm, name + '.png')
    def move(self, dx, dy):
        # relative mouse: chunk moves so no packet saturates
        while dx or dy:
            sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy)); self.hmp(f'mouse_move {sx} {sy}'); dx -= sx; dy -= sy
    def button(self, b): self.hmp(f'mouse_button {b}')
    def close(self):
        try: self.p.stdin.write(b'poweroff\n'); self.p.stdin.flush()
        except Exception: pass
        time.sleep(1); self.p.kill(); time.sleep(0.5)
        return self.out
if __name__ == '__main__':
    s = Session()
    w, h, pix = s.shot(sys.argv[1]); print(w, h); s.close()
