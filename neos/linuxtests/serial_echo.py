"""Opens every serial port NEOS offers (COM3 and USB serial adapters), reads one line from each and answers it, the way a pyserial based tool would."""
import os, termios, select, time

def setup(fd, baud):
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | baud; a[3] = 0
    a[4] = a[5] = baud; a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)

ports = {}
for path in ("/dev/ttyS2", "/dev/ttyUSB0", "/dev/ttyACM0"):
    if not os.path.exists(path): continue
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    setup(fd, termios.B115200); ports[fd] = path
print("SER ready", sorted(ports.values()), flush=True)
buf = {fd: b"" for fd in ports}; done = set(); deadline = time.time() + 90
while time.time() < deadline and len(done) < len(ports):
    r, _, _ = select.select(list(ports), [], [], 1)
    for fd in r:
        buf[fd] += os.read(fd, 256)
        while b"\n" in buf[fd]:
            line, buf[fd] = buf[fd].split(b"\n", 1)
            os.write(fd, b"ECHO " + line.upper() + b"\n"); done.add(fd)
            print("SER got", ports[fd], line, flush=True)
print("SER done", len(done), "of", len(ports), flush=True)
