#!/usr/bin/env python3
"""Build the Python runtime packages shipped with NEOS (offline package cache):

    tools/mkpython.py <out-dir>

  glibc-2.39.npk   the glibc loader and runtime libraries, installed under /lib64 and /lib/x86_64-linux-gnu so ANY dynamically linked
                   Linux program runs (Python, pip-built extensions, Debian packages)
  python3-<ver>.npk  CPython (this host's), the standard library as one compiled zip, extension modules and their libraries, pip,
                   and the `neos` module that gives Python programs the same tools the kernel gives AI agents

Both carry a .pkginfo so the OS can stage them from a boot module (`-initrd initrd.npk,glibc.npk,python3.npk`) and install them offline."""
import os, sys, subprocess, zipfile, io, compileall, py_compile, hashlib, glob, re, shutil, tempfile

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
PYVER = "%d.%d" % sys.version_info[:2]
PYBIN = os.path.realpath(sys.executable)
LIBDIR = f"/usr/lib/python{PYVER}"

def npk(files, info):
    b = bytearray(b"NPK1\n")
    allf = dict(files); allf[".pkginfo"] = "\n".join(f"{k}={v}" for k, v in info.items()).encode() + b"\n"
    for rel, data in allf.items():
        b += f"F {rel} {len(data)}\n".encode() + data + b"\n"
    b += b"END\n"
    return bytes(b)

def ldd(path):
    res = {}
    r = subprocess.run(["ldd", path], capture_output=True, text=True).stdout
    for line in r.splitlines():
        m = re.match(r"\s*(\S+) => (/\S+)", line)
        if m: res[m.group(1)] = os.path.realpath(m.group(2))
    return res

GLIBC = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "libutil.so.1", "libresolv.so.2", "libgcc_s.so.1", "libstdc++.so.6", "libz.so.1"}

# ---- glibc runtime
gfiles = {}
loader = "/lib64/ld-linux-x86-64.so.2"
gfiles[loader] = open(os.path.realpath(loader), "rb").read()
for name in ["libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "libutil.so.1", "libresolv.so.2", "libnss_files.so.2", "libnss_dns.so.2", "libgcc_s.so.1", "libz.so.1", "libstdc++.so.6"]:
    for d in ("/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            gfiles["/lib/x86_64-linux-gnu/" + name] = open(os.path.realpath(p), "rb").read(); break
gl = subprocess.run(["ldd", "--version"], capture_output=True, text=True).stdout.splitlines()[0].split()[-1]
open(os.path.join(out, "glibc.npk"), "wb").write(npk(gfiles, {"name": "glibc", "version": gl, "description": "glibc loader + runtime libraries: runs dynamically linked Linux programs"}))

# ---- CA certificates (HTTPS for Python / pip: OpenSSL inside the python package verifies server certificates against these)
ca = "/etc/ssl/certs/ca-certificates.crt"
if os.path.exists(ca):
    d = open(ca, "rb").read()
    open(os.path.join(out, "ca-certificates.npk"), "wb").write(npk({"/etc/ssl/certs/ca-certificates.crt": d}, {"name": "ca-certificates", "version": "2026", "description": "trusted root certificates (copied from the build host)"}))

# ---- python
files = {}
files["bin/python3"] = open(PYBIN, "rb").read()
skip_dirs = {"test", "tests", "idlelib", "tkinter", "turtledemo", "lib2to3", "__pycache__", "ensurepip", "venv", "pydoc_data", "site-packages", "dist-packages", "config-3.13-x86_64-linux-gnu"}
zbuf = io.BytesIO()
with zipfile.ZipFile(zbuf, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    for root, dirs, fs in os.walk(LIBDIR):
        dirs[:] = [d for d in dirs if d not in skip_dirs and d != "lib-dynload"]
        for f in fs:
            if not f.endswith(".py"): continue
            full = os.path.join(root, f); rel = os.path.relpath(full, LIBDIR)
            try:
                pyc = tempfile.mktemp(suffix=".pyc")
                py_compile.compile(full, cfile=pyc, dfile=rel, doraise=True, optimize=0, invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
                zf.write(pyc, rel[:-3] + ".pyc"); os.remove(pyc)
            except Exception as e:
                zf.write(full, rel)
    for f in ("_sysconfigdata__linux_x86_64-linux-gnu.py",):
        pass
files[f"lib/python{PYVER.replace('.', '')}.zip"] = zbuf.getvalue()
# extension modules + the libraries they need
want = ("_ssl", "_hashlib", "_ctypes", "_bz2", "_lzma", "_decimal", "_json", "_sqlite3", "_uuid", "_zoneinfo", "_asyncio", "_queue", "_multiprocessing", "_posixshmem", "_contextvars", "mmap", "resource", "termios", "_codecs_cn", "_codecs_hk", "_codecs_iso2022", "_codecs_jp", "_codecs_kr", "_codecs_tw", "_multibytecodec", "_lsprof", "_dbm")
libs = {}
for so in sorted(glob.glob(f"{LIBDIR}/lib-dynload/*.so")):
    base = os.path.basename(so).split(".")[0]
    if base not in want: continue
    files[f"lib/python{PYVER}/lib-dynload/{os.path.basename(so)}"] = open(so, "rb").read()
    for soname, real in ldd(so).items():
        if soname not in GLIBC: libs[soname] = real
for soname, real in ldd(PYBIN).items():
    if soname not in GLIBC: libs[soname] = real
for soname, real in libs.items():
    files["/lib/x86_64-linux-gnu/" + soname] = open(real, "rb").read()
# pip travels as its wheel (importable from a zip) found through a .pth file
whl = glob.glob(f"{LIBDIR}/ensurepip/_bundled/pip-*.whl")[0]
files["lib/" + os.path.basename(whl)] = open(whl, "rb").read()
files[f"local/lib/python{PYVER}/dist-packages/neos-pip.pth"] = f"/opt/python3/lib/{os.path.basename(whl)}\n".encode()
files[f"local/lib/python{PYVER}/dist-packages/neos.py"] = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "userland", "python", "neos.py"), "rb").read()
files[f"local/lib/python{PYVER}/dist-packages/README.txt"] = b"pip installs packages here (kept across reboots by NeoFS).\n"
files["bin/pip"] = b"#!/opt/python3/bin/python3\nimport sys\nfrom pip._internal.cli.main import main\nsys.exit(main())\n"
files["bin/pip3"] = files["bin/pip"]
pv = subprocess.run([PYBIN, "--version"], capture_output=True, text=True).stdout.split()[-1]
open(os.path.join(out, "python3.npk"), "wb").write(npk(files, {"name": "python3", "version": pv, "description": "CPython + pip + stdlib + the neos module (tools, settings, packages)", "depends": "glibc,ca-certificates" if os.path.exists(ca) else "glibc"}))
print("built", sorted(os.listdir(out)), {k: len(v) for k, v in [("python files", files)]})
