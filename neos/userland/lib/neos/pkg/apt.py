#!/opt/python3/bin/python3
"""apt-lite: install Debian / Ubuntu packages on NEOS.

    pkg install apt:<name>...      resolve Depends, download from the archive, verify SHA-256 (from the Packages index), unpack under /opt/apt
    pkg remove  apt:<name>
    pkg install apt:update         refresh the package index        (also: apt:search <text>, apt:show <name>)

Configuration: environment or /etc/neos/apt.conf (key=value): mirror, dist, components, arch.
Honest limits: the archive index is NOT authenticated here (no GnuPG): over plain HTTP anybody on the path can alter both the index and the
packages. Pin the checksum of the Packages index with `release_sha256=` in apt.conf (or APT_RELEASE_SHA256) when you can obtain it out of band.
Programs are unpacked into /opt/apt and reached through wrappers in /opt/bin that point the loader at that tree; zstd-compressed .deb members
are not supported (no zstd in the Python standard library yet)."""
import os, sys, io, json, gzip, lzma, bz2, hashlib, tarfile, urllib.request, re, shutil

STATE = "/var/lib/neos-apt"
PREFIX = "/opt/apt"
SYSTEM = {"libc6", "libc-bin", "libgcc-s1", "libstdc++6", "zlib1g", "gcc-13-base", "gcc-14-base", "libc6-dev", "base-files", "dpkg", "debconf", "perl-base", "tzdata"}

def conf():
    c = {"mirror": "http://archive.ubuntu.com/ubuntu", "dist": "noble", "components": "main universe", "arch": "amd64", "release_sha256": ""}
    try:
        for line in open("/etc/neos/apt.conf"):                      # key=value pairs, one per line or several per line (spaces inside a value: use quotes-free keys)
            if line.startswith("#"): continue
            for tok in re.findall(r"(\w+)=(\S*)", line):
                c[tok[0]] = tok[1]
        c["components"] = c["components"].replace(",", " ")
    except OSError: pass
    for k in list(c):
        v = os.environ.get("APT_" + k.upper())
        if v: c[k] = v
    return c

def say(*a): print("pkg:", *a, flush=True)
def die(*a): print("pkg:", *a, file=sys.stderr, flush=True); sys.exit(1)

def get(url):
    try:
        with urllib.request.urlopen(url, timeout=60) as r: return r.read()
    except Exception as e: die(f"cannot download {url}: {e}")

def parse_packages(text):
    pk = {}
    for block in text.split("\n\n"):
        d = {}; key = None
        for line in block.split("\n"):
            if line.startswith((" ", "\t")) and key: d[key] += "\n" + line.strip(); continue
            if ":" in line: key, v = line.split(":", 1); d[key] = v.strip()
        if "Package" in d and d["Package"] not in pk: pk[d["Package"]] = d
    return pk

def update(c):
    os.makedirs(STATE, exist_ok=True)
    allp = {}
    for comp in c["components"].split():
        base = f"{c['mirror']}/dists/{c['dist']}/{comp}/binary-{c['arch']}"
        data = None
        for name, dec in (("Packages.xz", lzma.decompress), ("Packages.gz", gzip.decompress), ("Packages", lambda b: b)):
            try:
                with urllib.request.urlopen(f"{base}/{name}", timeout=60) as r: raw = r.read()
                if c["release_sha256"] and hashlib.sha256(raw).hexdigest() != c["release_sha256"] and comp == c["components"].split()[0]:
                    die(f"{name} SHA-256 does not match release_sha256")
                data = dec(raw); break
            except Exception: continue
        if data is None: say(f"no index for {comp}"); continue
        for k, v in parse_packages(data.decode("utf-8", "replace")).items(): allp.setdefault(k, v)
        say(f"{comp}: index read")
    if not allp: die("no package index could be read (mirror/dist/components in /etc/neos/apt.conf)")
    slim = {k: {f: v[f] for f in ("Version", "Filename", "SHA256", "Depends", "Pre-Depends", "Provides", "Description", "Size") if f in v} for k, v in allp.items()}
    json.dump(slim, open(f"{STATE}/index.json", "w"))
    say(f"{len(slim)} packages indexed")

def load_index():
    try: return json.load(open(f"{STATE}/index.json"))
    except OSError: die("no index yet: run `pkg install apt:update`")

def provides_map(idx):
    pm = {}
    for n, d in idx.items():
        for p in d.get("Provides", "").split(","):
            p = p.strip().split(" ")[0]
            if p: pm.setdefault(p, n)
    return pm

def installed(): return {f[:-5] for f in os.listdir(STATE) if f.endswith(".list")} if os.path.isdir(STATE) else set()

def resolve(want, idx, have):
    pm = provides_map(idx); order = []; seen = set()
    def visit(n, why):
        if n in seen or n in SYSTEM or n in have: return
        if n not in idx:
            if n in pm: return visit(pm[n], why)
            die(f"package {n} not found ({why}); `pkg install apt:update` first?")
        seen.add(n)
        deps = (idx[n].get("Pre-Depends", "") + "," + idx[n].get("Depends", "")).strip(",")
        for dep in deps.split(","):
            alt = dep.split("|")[0].strip().split(" ")[0].split(":")[0]
            if alt: visit(alt, f"needed by {n}")
        order.append(n)
    for n in want: visit(n, "requested")
    return order

def ar_members(blob):
    if blob[:8] != b"!<arch>\n": die("not a .deb (ar) file")
    i = 8; out = {}
    while i + 60 <= len(blob):
        name = blob[i:i + 16].decode().strip().rstrip("/"); size = int(blob[i + 48:i + 58]); i += 60
        out[name] = blob[i:i + size]; i += size + (size & 1)
    return out

def unpack(name, blob):
    m = ar_members(blob); files = []
    data = next((k for k in m if k.startswith("data.tar")), None)
    if not data: die(f"{name}: no data member")
    if data.endswith(".zst"): die(f"{name}: zstd-compressed .deb members are not supported yet (use an xz/gz mirror)")
    with tarfile.open(fileobj=io.BytesIO(m[data]), mode="r:*") as tf:
        for t in tf:
            p = t.name.lstrip("./")
            if not p or p.startswith("..") or "/../" in p: continue
            dst = os.path.join(PREFIX, p)
            if t.isdir(): os.makedirs(dst, exist_ok=True); continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.lexists(dst): os.remove(dst)
            if t.issym(): os.symlink(t.linkname, dst); files.append(dst)
            elif t.isfile():
                with open(dst, "wb") as f: shutil.copyfileobj(tf.extractfile(t), f)
                os.chmod(dst, t.mode & 0o7777 | 0o200); files.append(dst)
    return files

def wrappers(files, pkg):
    os.makedirs("/opt/bin", exist_ok=True); made = []
    libs = f"{PREFIX}/usr/lib/x86_64-linux-gnu:{PREFIX}/lib/x86_64-linux-gnu:{PREFIX}/usr/lib"
    for f in files:
        d = os.path.dirname(f)
        if d in (f"{PREFIX}/usr/bin", f"{PREFIX}/bin", f"{PREFIX}/usr/sbin", f"{PREFIX}/sbin") and os.path.isfile(f) and os.access(f, os.X_OK):
            w = "/opt/bin/" + os.path.basename(f)
            with open(w, "w") as o: o.write(f'#!/bin/sh\nexport LD_LIBRARY_PATH={libs}:$LD_LIBRARY_PATH\nexec {f} "$@"\n')
            os.chmod(w, 0o755); made.append(w)
    return made

def neosctl(*a):
    import subprocess
    subprocess.run(["/bin/neosctl", *a], stdout=subprocess.DEVNULL)

def install(names, c):
    idx = load_index(); have = installed()
    order = resolve(names, idx, have)
    if not order: say("nothing to do: already installed"); return
    say("will install:", " ".join(f"{n}={idx[n]['Version']}" for n in order))
    os.makedirs(STATE, exist_ok=True)
    for n in order:
        d = idx[n]; url = f"{c['mirror']}/{d['Filename']}"
        say(f"fetching {n} {d['Version']}")
        blob = get(url)
        if hashlib.sha256(blob).hexdigest() != d["SHA256"]: die(f"SHA-256 mismatch for {n}: corrupted or tampered package")
        files = unpack(n, blob)
        made = wrappers(files, n)
        json.dump(files + made, open(f"{STATE}/{n}.list", "w"))
        neosctl("pkg.record", n, d["Version"], "apt")
        say(f"installed {n} {d['Version']} ({len(files)} files, {len(made)} programs)")

def remove(names):
    have = installed(); owned = {}
    for n in have:
        for f in json.load(open(f"{STATE}/{n}.list")): owned.setdefault(f, set()).add(n)
    for n in names:
        if n not in have: die(f"{n} is not installed")
        for f in json.load(open(f"{STATE}/{n}.list")):
            if owned.get(f, set()) - {n}: continue
            try: os.remove(f)
            except OSError: pass
        for p in (f"{STATE}/{n}.list", f"/var/pkg/{n}"):
            try: os.remove(p)
            except OSError: pass
        say(f"removed {n}")

def main():
    a = sys.argv[1:]
    if not a: die("usage: install|remove|update|search|show ...")
    sub, rest = a[0], a[1:]; c = conf()
    if sub == "install" and rest == ["update"] or sub == "update": return update(c)
    if sub == "install": return install(rest, c)
    if sub == "remove": return remove(rest)
    if sub == "search":
        idx = load_index()
        for n, d in sorted(idx.items()):
            if rest and rest[0] in n: print(f"{n} {d['Version']} - {d.get('Description', '').splitlines()[0] if d.get('Description') else ''}")
        return
    if sub == "show":
        d = load_index().get(rest[0]) or die("unknown package"); print(json.dumps(d, indent=1)); return
    die("usage: install|remove|update|search|show ...")
main()
