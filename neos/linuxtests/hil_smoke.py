"""What a Python HiL / test-bench application needs from the OS, one probe each. Runs on NEOS and on a normal Linux host."""
import sys, os, time, threading, socket, struct, json, queue, tempfile, importlib

results = []
def probe(name):
    def deco(fn):
        print(f"HIL ...  {name}", flush=True)
        try:
            detail = fn()
            results.append((name, True, detail or ""))
        except Exception as e:
            results.append((name, False, f"{type(e).__name__}: {e}"))
        return fn
    return deco

@probe("threads+queue")
def _():
    q = queue.Queue(); ts = [threading.Thread(target=lambda i=i: q.put(i * i)) for i in range(8)]
    [t.start() for t in ts]; [t.join() for t in ts]
    assert sorted(q.get() for _ in range(8)) == [i * i for i in range(8)]

@probe("periodic loop jitter")
def _():
    period, n, worst = 0.005, 40, 0.0; t0 = time.perf_counter(); nxt = t0
    for _ in range(n):
        nxt += period; d = nxt - time.perf_counter()
        if d > 0: time.sleep(d)
        worst = max(worst, time.perf_counter() - nxt)
    assert time.perf_counter() - t0 < n * period * 3
    return f"worst lateness {worst * 1000:.1f} ms"

@probe("tcp loopback")
def _():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1); port = s.getsockname()[1]
    def srv():
        c, _ = s.accept(); c.sendall(c.recv(64).upper()); c.close()
    t = threading.Thread(target=srv); t.start()
    c = socket.create_connection(("127.0.0.1", port)); c.sendall(b"hil ready"); r = c.recv(64); c.close(); t.join()
    assert r == b"HIL READY"

@probe("udp loopback")
def _():
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.bind(("127.0.0.1", 0)); b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b.sendto(struct.pack("<Id", 7, 3.5), a.getsockname()); a.settimeout(2); d, _ = a.recvfrom(64)
    assert struct.unpack("<Id", d) == (7, 3.5)

@probe("select/epoll")
def _():
    import selectors
    a, b = socket.socketpair(); sel = selectors.DefaultSelector(); sel.register(a, selectors.EVENT_READ)
    b.send(b"x"); assert sel.select(2); return type(sel).__name__

@probe("sqlite3")
def _():
    import sqlite3
    db = sqlite3.connect(":memory:"); db.execute("create table m(t real, v real)"); db.executemany("insert into m values(?,?)", [(i, i * .5) for i in range(100)])
    assert db.execute("select sum(v) from m").fetchone()[0] == 2475.0

@probe("json/csv/zlib/hashlib")
def _():
    import csv, zlib, hashlib, io
    assert json.loads(json.dumps({"a": [1, 2]})) == {"a": [1, 2]}
    buf = io.StringIO(); csv.writer(buf).writerow([1, "x"]); assert zlib.decompress(zlib.compress(b"abc" * 100)) == b"abc" * 100
    hashlib.sha256(b"x").hexdigest()

@probe("files+mmap")
def _():
    import mmap
    with tempfile.NamedTemporaryFile() as f:
        f.write(b"\0" * 8192); f.flush(); m = mmap.mmap(f.fileno(), 8192); m[0:4] = b"HIL!"; assert m[0:4] == b"HIL!"; m.close()

@probe("ctypes dlopen libc")
def _():
    import ctypes, ctypes.util
    libc = ctypes.CDLL(None); assert libc.abs(-5) == 5; return str(ctypes.util.find_library("c"))

@probe("subprocess")
def _():
    import subprocess
    assert subprocess.run([sys.executable, "-c", "print(6*7)"], capture_output=True, text=True).stdout.strip() == "42"

@probe("multiprocessing fork")
def _():
    import multiprocessing as mp
    ctx = mp.get_context("fork"); q = ctx.Queue(); p = ctx.Process(target=lambda: q.put(os.getpid())); p.start(); v = q.get(timeout=10); p.join()
    assert v != os.getpid()

@probe("multiprocessing shared memory + lock")
def _():
    import multiprocessing as mp
    from multiprocessing import shared_memory
    ctx = mp.get_context("fork"); shm = shared_memory.SharedMemory(create=True, size=4096); v = ctx.Value("i", 0); lk = ctx.Lock()
    def child():
        shm.buf[0:5] = b"hello"
        for _ in range(100):
            with lk: v.value += 1
    ps = [ctx.Process(target=child) for _ in range(3)]
    [p.start() for p in ps]; [p.join(30) for p in ps]
    ok = bytes(shm.buf[0:5]) == b"hello" and v.value == 300; shm.close(); shm.unlink()
    assert ok, (bytes(shm.buf[0:5]) if False else "shared data not visible", v.value)

@probe("asyncio")
def _():
    import asyncio
    async def main():
        r, w = await asyncio.open_connection(*srv_addr); w.write(b"ping\n"); await w.drain(); d = await r.readline(); w.close(); return d
    async def serve():
        global srv_addr
        async def h(r, w): w.write(await r.readline()); await w.drain(); w.close()
        s = await asyncio.start_server(h, "127.0.0.1", 0); srv_addr = s.sockets[0].getsockname()[:2]
        d = await main(); s.close(); return d
    assert asyncio.run(serve()) == b"ping\n"

@probe("http.server+urllib")
def _():
    import http.server, urllib.request
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self): self.send_response(200); self.end_headers(); self.wfile.write(b"ok")
        def log_message(self, *a): pass
    s = http.server.HTTPServer(("127.0.0.1", 0), H); threading.Thread(target=s.serve_forever, daemon=True).start()
    assert urllib.request.urlopen(f"http://127.0.0.1:{s.server_port}/", timeout=5).read() == b"ok"; s.shutdown()

@probe("realtime scheduling (SCHED_FIFO)")
def _():
    try:
        os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(50)); return "SCHED_FIFO 50 granted"
    except PermissionError:
        return "denied (EPERM) - same as a non-root Linux user"

@probe("memory lock (mlockall)")
def _():
    import ctypes; libc = ctypes.CDLL(None, use_errno=True)
    r = libc.mlockall(3); return "ok" if r == 0 else f"errno {ctypes.get_errno()}"

@probe("serial port stack (termios, /dev/tty*)")
def _():
    import termios; return "termios present, " + (", ".join(sorted(f for f in os.listdir("/dev") if f.startswith("tty"))[:4]) or "no /dev/tty*")

@probe("raw USB / device nodes (/dev/bus/usb)")
def _():
    return "present" if os.path.isdir("/dev/bus/usb") else "MISSING: no /dev/bus/usb (libusb / pyusb / NI-VISA USB cannot see devices)"

@probe("network interface listing (optional)")
def _():
    names = [n for _, n in socket.if_nameindex()]
    return "interfaces: " + ",".join(names)

@probe("numpy (optional)")
def _():
    import numpy as np; a = np.arange(1000.0); assert abs(float(np.fft.rfft(a).real[0]) - 499500.0) < 1e-6; return np.__version__

@probe("pure-python GUI toolkit availability (optional)")
def _():
    out = []
    for m in ("tkinter", "PySide6", "PyQt5", "PyQt6", "wx", "kivy"):
        try: importlib.import_module(m); out.append(m)
        except Exception: pass
    return "available: " + (", ".join(out) or "none (needs a display server/Wayland/linuxfb)")

OPTIONAL = {"network interface listing (optional)", "numpy (optional)", "pure-python GUI toolkit availability (optional)"}
bad = 0
for name, ok, detail in results:
    tag = "PASS" if ok else ("SKIP" if name in OPTIONAL else "FAIL")
    bad += tag == "FAIL"
    print(f"HIL {tag:4} {name}" + (f"  [{detail}]" if detail else ""))
print("HIL SMOKE OK" if not bad else f"HIL SMOKE FAILED ({bad})")
