#!/usr/bin/env python3
"""NEOS compatibility scanner: tells, before you run anything, what an application needs that NEOS does not (yet) provide.

  tools/compat-scan.py pe  <file.exe|file.dll|directory> ...   Windows programs/DLLs: import coverage against the winrt runtime
  tools/compat-scan.py py  <project directory> ...             Python projects: imports, requirements, GUI toolkit / driver / OS needs
  tools/compat-scan.py elf <file|directory> ...                Linux programs/libraries: needed shared objects, device/ioctl hints

Exit status 0 = nothing blocking found, 1 = at least one blocker."""
import os, sys, struct, re, subprocess, ast, json, collections, glob

HERE = os.path.dirname(os.path.abspath(__file__))
WINRT = os.path.join(HERE, '..', 'build', 'linux', 'winrt')

# ------------------------------------------------------------------ Windows (PE)
SYSTEM_DLLS_OK = {'kernel32.dll', 'kernelbase.dll', 'ntdll.dll', 'msvcrt.dll', 'ucrtbase.dll'}
GUI_DLLS = {'user32.dll', 'gdi32.dll', 'comctl32.dll', 'comdlg32.dll', 'shell32.dll', 'opengl32.dll', 'd3d9.dll', 'd3d11.dll', 'dxgi.dll', 'uxtheme.dll',
            'dwmapi.dll', 'win32u.dll', 'imm32.dll', 'winmm.dll', 'dsound.dll', 'd2d1.dll', 'dwrite.dll', 'glu32.dll', 'gdiplus.dll', 'msimg32.dll'}
SERVICE_DLLS = {'advapi32.dll', 'ole32.dll', 'oleaut32.dll', 'rpcrt4.dll', 'shlwapi.dll', 'ws2_32.dll', 'wsock32.dll', 'iphlpapi.dll', 'setupapi.dll', 'cfgmgr32.dll',
                'winspool.drv', 'version.dll', 'wininet.dll', 'winhttp.dll', 'crypt32.dll', 'bcrypt.dll', 'secur32.dll', 'psapi.dll', 'userenv.dll', 'netapi32.dll',
                'wtsapi32.dll', 'shcore.dll', 'propsys.dll', 'combase.dll', 'sechost.dll', 'dbghelp.dll', 'wevtapi.dll', 'powrprof.dll', 'comsvcs.dll', 'mpr.dll',
                'hid.dll', 'winusb.dll', 'wbemuuid.dll', 'ncrypt.dll', 'wintrust.dll', 'msi.dll', 'cabinet.dll', 'avrt.dll', 'ntmarta.dll'}

def shim_symbols():
    if not os.path.exists(WINRT): return None
    try: return set(subprocess.run([WINRT, '--symbols'], capture_output=True, text=True, timeout=20).stdout.split())
    except Exception: return None

def rva_to_off(secs, rva):
    for va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs): return raw + rva - va
    return None

def cstr(d, off):
    e = d.index(b'\0', off); return d[off:e].decode('latin1')

def parse_pe(path):
    d = open(path, 'rb').read()
    if d[:2] != b'MZ': return None
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0': return None
    machine, nsec, _, _, _, osz, chars = struct.unpack_from('<HHIIIHH', d, pe + 4)
    opt = pe + 24; magic = struct.unpack_from('<H', d, opt)[0]; p64 = magic == 0x20B
    ddir = opt + (112 if p64 else 96)
    secs = []
    for i in range(nsec):
        s = opt + osz + i * 40
        vs, va, rs, raw = struct.unpack_from('<IIII', d, s + 8); secs.append((va, vs, raw, rs))
    info = {'path': path, 'machine': machine, 'pe64': p64, 'dll': bool(chars & 0x2000), 'imports': collections.OrderedDict(), 'delay': collections.OrderedDict(),
            'exports': [], 'subsystem': struct.unpack_from('<H', d, opt + 68)[0], 'managed': False}
    def read_imports(dirrva, into, delay=False):
        off = rva_to_off(secs, dirrva)
        if off is None: return
        while True:
            if delay:
                attrs, name, mod, iat, int_ = struct.unpack_from('<IIIII', d, off)[0:5]
                if not name: break
                thunk = int_; nm = name
                off += 32
            else:
                oft, _, _, nm, ft = struct.unpack_from('<IIIII', d, off)
                if not nm: break
                thunk = oft or ft; off += 20
            dll = cstr(d, rva_to_off(secs, nm)).lower(); names = into.setdefault(dll, [])
            t = rva_to_off(secs, thunk); sz = 8 if p64 else 4
            while t is not None:
                v = int.from_bytes(d[t:t + sz], 'little')
                if not v: break
                if v >> (sz * 8 - 1): names.append('#%d' % (v & 0xFFFF))
                else:
                    o = rva_to_off(secs, v & 0x7FFFFFFF); names.append(cstr(d, o + 2) if o is not None else '?')
                t += sz
    imp_rva, imp_sz = struct.unpack_from('<II', d, ddir + 8)
    if imp_sz: read_imports(imp_rva, info['imports'])
    dl_rva, dl_sz = struct.unpack_from('<II', d, ddir + 13 * 8)
    if dl_sz: read_imports(dl_rva, info['delay'], True)
    clr_rva, clr_sz = struct.unpack_from('<II', d, ddir + 14 * 8)
    info['managed'] = bool(clr_sz)
    exp_rva, exp_sz = struct.unpack_from('<II', d, ddir)
    if exp_sz:
        o = rva_to_off(secs, exp_rva)
        if o is not None:
            nn, = struct.unpack_from('<I', d, o + 24); names_rva, = struct.unpack_from('<I', d, o + 32); no = rva_to_off(secs, names_rva)
            for i in range(nn):
                r, = struct.unpack_from('<I', d, no + 4 * i); info['exports'].append(cstr(d, rva_to_off(secs, r)))
    return info

def gather(paths, exts):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for r, _, fs in os.walk(p):
                out += [os.path.join(r, f) for f in fs if f.lower().endswith(exts)]
        elif os.path.exists(p): out.append(p)
    return sorted(out)

def scan_pe(paths):
    files = gather(paths, ('.exe', '.dll', '.sys', '.ocx', '.cpl'))
    syms = shim_symbols()
    if syms is None: print('note: build first (make) - build/linux/winrt lists what the runtime provides; assuming nothing is provided\n'); syms = set()
    infos = [i for i in (parse_pe(f) for f in files) if i]
    local = {os.path.basename(i['path']).lower(): i for i in infos}
    blockers = 0; total_imp = total_ok = 0; missing = collections.Counter(); dll_missing = collections.defaultdict(set)
    for i in infos:
        notes = []
        if not i['pe64']: notes.append('32-bit PE (NEOS runs 64-bit only)'); blockers += 1
        if i['managed']: notes.append('.NET assembly (needs a CLR: not available)'); blockers += 1
        uses = list(i['imports'].items()) + list(i['delay'].items())
        gui = sorted(d for d, _ in uses if d in GUI_DLLS)
        if gui and i['subsystem'] == 2: notes.append('GUI program (Win32 windowing is not implemented): ' + ', '.join(gui)); blockers += 1
        elif gui: notes.append('uses GUI libraries: ' + ', '.join(gui))
        n_ok = n_all = 0
        for dll, names in uses:
            if dll in local: continue                                    # satisfied by a DLL that ships next to it
            for n in names:
                n_all += 1
                if n in syms: n_ok += 1
                else: missing[(dll, n)] += 1; dll_missing[dll].add(n)
        total_imp += n_all; total_ok += n_ok
        kind = 'DLL' if i['dll'] else 'EXE'
        print(f"{kind} {i['path']}: {n_ok}/{n_all} imports provided" + (f"  ({len(i['exports'])} exports)" if i['dll'] else ''))
        for n in notes: print('   !', n)
    if not infos: print('no PE files found'); return 1
    print(f"\nTOTAL: {total_ok}/{total_imp} external imports provided by winrt ({100 * total_ok // max(total_imp, 1)}%), the rest become stubs that stop the program when called (WINRT_LENIENT=1 logs and returns 0 instead)")
    if dll_missing:
        print('\nMissing, by DLL (most numerous first):')
        for dll, ns in sorted(dll_missing.items(), key=lambda kv: -len(kv[1]))[:25]:
            tag = 'GUI' if dll in GUI_DLLS else 'service' if dll in SERVICE_DLLS else 'system' if dll in SYSTEM_DLLS_OK else 'other/private'
            print(f"  {dll:34} {len(ns):5} names  [{tag}]  e.g. {', '.join(sorted(ns)[:4])}")
    print('\nVERDICT:', 'BLOCKED - see ! lines' if blockers else ('runs only if every missing name above is never called' if missing else 'all imports are provided'))
    return 1 if blockers else 0

# ------------------------------------------------------------------ Python projects
STDLIB_OK = set(sys.stdlib_module_names) - {'tkinter', 'idlelib', 'turtle', 'turtledemo', 'winreg', 'winsound', 'msvcrt', '_winapi', 'nt', 'ossaudiodev', 'curses'}
WHEELS_OK = {'numpy', 'scipy', 'pandas', 'matplotlib', 'sympy', 'requests', 'urllib3', 'certifi', 'yaml', 'pyyaml', 'jinja2', 'markupsafe', 'flask', 'werkzeug', 'click',
             'fastapi', 'starlette', 'uvicorn', 'pydantic', 'aiohttp', 'websockets', 'websocket', 'serial', 'pyserial', 'h5py', 'tables', 'openpyxl', 'xlsxwriter',
             'lxml', 'bs4', 'cryptography', 'paramiko', 'psutil', 'sqlalchemy', 'zmq', 'pyzmq', 'msgpack', 'protobuf', 'google', 'grpc', 'paho', 'pymodbus', 'opcua', 'asyncua',
             'can', 'cantools', 'dateutil', 'pytz', 'tqdm', 'rich', 'attr', 'attrs', 'toml', 'tomli', 'numba', 'sklearn', 'scikit_learn', 'networkx', 'control', 'simpy'}
NEED = {
    'GUI toolkit (needs a window system; NEOS has its own GUI and no X11/Wayland/Qt platform plugin)': {'PyQt5', 'PyQt6', 'PySide2', 'PySide6', 'tkinter', 'wx', 'kivy', 'pygame', 'pyqtgraph', 'dearpygui', 'customtkinter', 'vispy', 'glfw', 'OpenGL'},
    'Windows-only API (needs Win32 COM/registry, not an ELF/Linux library)': {'win32api', 'win32com', 'win32con', 'win32gui', 'pythoncom', 'pywintypes', 'wmi', 'winreg', '_winreg', 'msvcrt', 'winsound', 'comtypes', 'pywinauto', 'clr', 'System'},
    'NI driver stack (proprietary libnidaqmx/NI-VISA: not installable)': {'nidaqmx', 'nifgen', 'niscope', 'nidmm', 'nidcpower', 'nise', 'nitclk', 'niswitch', 'nixnet', 'nidigital', 'nimodinst', 'ni_xnet'},
    'VISA / instrument I/O (needs a VISA backend: pyvisa-py works for TCP/serial, NI-VISA does not)': {'pyvisa', 'visa'},
    'USB / HID / SocketCAN device access (needs /dev/bus/usb, /dev/hidraw, AF_CAN: not in NEOS yet)': {'usb', 'pyusb', 'hid', 'hidapi', 'libusb1', 'usb1', 'serial.tools.list_ports_linux'},
    'Browser-delivered UI that depends on JavaScript (the NEOS browser renders HTML/forms only)': {'dash', 'streamlit', 'gradio', 'nicegui', 'panel', 'bokeh', 'plotly', 'flet', 'eel', 'webview', 'pywebview', 'jupyter', 'notebook', 'ipywidgets'},
    'Heavy native stacks (CUDA/GPU/OpenCV windows): no GPU driver in NEOS': {'torch', 'tensorflow', 'jax', 'cv2', 'cupy', 'pycuda', 'pyopencl', 'vtk'},
}
WARN_NEED = {'GUI toolkit', 'Windows-only', 'NI driver', 'USB / HID', 'Heavy native'}

def top_imports(root):
    mods = collections.defaultdict(list); local = set()
    for r, ds, fs in os.walk(root):
        ds[:] = [d for d in ds if d not in ('.git', '__pycache__', 'node_modules', '.venv', 'venv', 'build', 'dist')]
        for f in fs:
            if f.endswith('.py'): local.add(f[:-3])
        for d in ds: local.add(d)
    for r, ds, fs in os.walk(root):
        ds[:] = [d for d in ds if d not in ('.git', '__pycache__', 'node_modules', '.venv', 'venv', 'build', 'dist')]
        for f in fs:
            if not f.endswith('.py'): continue
            p = os.path.join(r, f)
            try: tree = ast.parse(open(p, encoding='utf-8', errors='replace').read())
            except SyntaxError: continue
            for n in ast.walk(tree):
                names = []
                if isinstance(n, ast.Import): names = [a.name for a in n.names]
                elif isinstance(n, ast.ImportFrom) and n.module and n.level == 0: names = [n.module]
                for nm in names:
                    top = nm.split('.')[0]
                    if top not in local: mods[top].append(os.path.relpath(p, root))
    return mods

def requirement_names(root):
    out = set()
    for f in glob.glob(os.path.join(root, '**', 'requirements*.txt'), recursive=True):
        for ln in open(f, errors='replace'):
            ln = ln.split('#')[0].strip()
            m = re.match(r'([A-Za-z0-9_.\-]+)', ln)
            if m and not ln.startswith('-'): out.add(m.group(1).lower().replace('-', '_'))
    pp = os.path.join(root, 'pyproject.toml')
    if os.path.exists(pp):
        for m in re.finditer(r'"([A-Za-z0-9_.\-]+)\s*(?:[<>=!~\[;].*)?"', open(pp, errors='replace').read()): out.add(m.group(1).lower().replace('-', '_'))
    return out

def scan_py(paths):
    blockers = 0
    for root in paths:
        mods = top_imports(root); reqs = requirement_names(root)
        print(f"Python project {root}: {len(mods)} third-party/stdlib modules imported, {len(reqs)} requirements listed")
        names = set(mods) | reqs
        found = collections.defaultdict(list)
        for cat, mset in NEED.items():
            hit = sorted(n for n in names if n in mset or n.replace('_', '-') in mset or n.lower() in {m.lower() for m in mset})
            if hit: found[cat] = hit
        for cat, hit in found.items():
            print(f"  ! {cat}\n      {', '.join(hit)}")
            if any(cat.startswith(w) for w in WARN_NEED): blockers += 1
        ok = sorted(n for n in names if n in STDLIB_OK or n.lower() in WHEELS_OK)
        flagged = {n for h in found.values() for n in h}
        unk = sorted(n for n in names if n not in flagged and n not in STDLIB_OK and n.lower() not in WHEELS_OK)
        print(f"  ok  ({len(ok)}): standard library / pure-Python or manylinux wheels that install with `pip install` on NEOS")
        if unk: print(f"  ?   ({len(unk)}) not classified - check that a pure-Python or manylinux_x86_64 wheel exists: {', '.join(unk[:30])}{' ...' if len(unk) > 30 else ''}")
        if re.search(r'ctypes\.(windll|WinDLL|oledll)', ''.join(open(os.path.join(r, f), errors='replace').read() for r, _, fs in os.walk(root) for f in fs if f.endswith('.py'))):
            print('  ! ctypes.windll / WinDLL: loads Windows DLLs from Python; only works through the winrt layer if the DLL itself is a plain 64-bit PE'); blockers += 1
    print('\nVERDICT:', 'BLOCKED - see ! lines' if blockers else 'no blocker found (run it, then use linuxtests/hil_smoke.py to probe the OS features it relies on)')
    return 1 if blockers else 0

# ------------------------------------------------------------------ ELF
def scan_elf(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for r, _, fs in os.walk(p):
                for f in fs:
                    q = os.path.join(r, f)
                    try:
                        if open(q, 'rb').read(4) == b'\x7fELF': files.append(q)
                    except OSError: pass
        elif os.path.exists(p): files.append(p)
    if not files: print('no ELF files found'); return 1
    have = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1', 'libutil.so.1', 'libresolv.so.2', 'libnss_files.so.2', 'ld-linux-x86-64.so.2'}
    hints = {b'/dev/bus/usb': 'raw USB (usbfs)', b'/sys/bus/usb': 'sysfs USB enumeration', b'/dev/ttyUSB': 'USB serial', b'/dev/hidraw': 'hidraw', b'AF_CAN': 'SocketCAN',
             b'/dev/mem': '/dev/mem', b'/sys/class': 'sysfs class devices', b'libX11': 'X11', b'wayland': 'Wayland', b'/dev/dri': 'DRM/GPU', b'/dev/snd': 'ALSA audio',
             b'libnidaqmx': 'NI-DAQmx', b'libvisa': 'NI-VISA', b'/dev/ni': 'NI kernel driver node', b'NiFpga': 'NI FPGA interface', b'/dev/kvm': 'KVM', b'/proc/sys/kernel': 'sysctl'}
    bad = 0
    for f in files:
        try: out = subprocess.run(['readelf', '-d', '-h', f], capture_output=True, text=True).stdout
        except FileNotFoundError: print('readelf not available'); return 1
        if 'X86-64' not in out and 'Advanced Micro Devices X86-64' not in out: print(f'{f}: not x86-64'); bad += 1; continue
        needed = re.findall(r'Shared library: \[(.+?)\]', out)
        miss = [n for n in needed if n not in have]
        data = open(f, 'rb').read()
        hit = sorted({v for k, v in hints.items() if k in data})
        print(f"{f}: needs {', '.join(needed) or '(static)'}")
        if miss: print('   shared objects to provide (pkg/apt/pip or ship them): ' + ', '.join(miss))
        for h in hit: print('   ! mentions', h)
        if any(h in ('X11', 'Wayland', 'DRM/GPU', 'ALSA audio', 'NI-DAQmx', 'NI kernel driver node', 'NI FPGA interface', 'KVM') for h in hit): bad += 1
    print('\nVERDICT:', 'BLOCKED - see ! lines' if bad else 'no blocker found')
    return 1 if bad else 0

if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] not in ('pe', 'py', 'elf'): print(__doc__); sys.exit(2)
    sys.exit({'pe': scan_pe, 'py': scan_py, 'elf': scan_elf}[sys.argv[1]](sys.argv[2:]))
