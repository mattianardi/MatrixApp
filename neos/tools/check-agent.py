#!/usr/bin/env python3
"""End-to-end test of the NEOS agent subsystem: boots NEOS in QEMU, plays the human operator on the
console (COM1) and an external AI agent on the agent channel (COM2)."""
import os, re, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(__file__))
from neos_agent import NeosAgent, NeosError

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sock = os.path.join(root, "build", "agent-test.sock")
if os.path.exists(sock):
    os.unlink(sock)
q = subprocess.Popen(["qemu-system-x86_64", "-kernel", os.path.join(root, "build/matrix32.elf"), "-initrd", os.path.join(root, "build/initrd.npk"), "-m", "512", "-display", "none",
                      "-cpu", "max", "-serial", "stdio", "-serial", f"unix:{sock},server,nowait", "-no-reboot", "-nic", "none"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
console = bytearray()
def pump():
    while True:
        b = q.stdout.read1(4096)
        if not b:
            return
        console.extend(b)
threading.Thread(target=pump, daemon=True).start()
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
def text():
    return ANSI.sub(b"", bytes(console)).decode(errors="replace")
def wait_for(s, timeout=20, start=0):
    end = time.time() + timeout
    while time.time() < end:
        t = text()
        i = t.find(s, start)
        if i >= 0:
            return i
        time.sleep(0.05)
    raise TimeoutError(f"timeout waiting for {s!r}\n--- console ---\n{text()[-1500:]}")
def op(cmd, wait=None, timeout=20):
    mark = len(text())
    q.stdin.write(cmd.encode() + b"\n"); q.stdin.flush()
    wait_for(wait or "neos> ", timeout, mark + len(cmd))
    time.sleep(0.15)
    return text()[mark:]

fails = []
def check(name, cond, extra=""):
    print(("  ok   " if cond else "  FAIL ") + name + (f"  [{extra}]" if (extra and not cond) else ""))
    if not cond:
        fails.append(name)

try:
    wait_for("neos> ", 40)
    out = op("agent add coder remote")
    token = re.search(r"token[^:]*:\s*([0-9a-f]{32})", out).group(1)
    op("agent grant coder observe tune:sched.quantum[2..20],timer.hz[500..4000] fs.read:/src/,/var/dist/,/opt/ fs.write:/src/ pkg:demo thread:minprio=4 experiment")

    a = NeosAgent.unix(sock)
    try:
        a.auth("coder", "0" * 32); check("wrong token rejected", False)
    except NeosError as e:
        check("wrong token rejected", e.code == 401)
    a.auth("coder", token)
    me = a.call("whoami")
    check("authenticated, identity and caps visible", me["agent"] == "coder" and "tune" in me["caps"])
    names = {t["name"] for t in a.tools()}
    check("tool schema only lists permitted tools", "tune.set" in names and "debug.peek" not in names and "net.get" not in names)

    # --- scope enforcement
    r = a.call("tune.set", "sched.quantum", "7")
    check("in-scope tune applied", r["applied"] and a.call("tune.get", "sched.quantum")["value"] == "7")
    for name, args in [("value above range", ("sched.quantum", "99")), ("name outside scope", ("heap.size_mb", "16")),
                       ("operator-only setting", ("pkg.allow_insecure", "on")), ("agent subsystem itself", ("agent.rate_per_min", "6000"))]:
        try:
            a.call("tune.set", *args); check(name + " refused", False)
        except NeosError as e:
            check(name + " refused", e.code == 403, str(e))
    try:
        a.call("var.set", "NEOS.heap.size_mb", "16"); check("NEOS.* variable cannot bypass tune scope", False)
    except NeosError as e:
        check("NEOS.* variable cannot bypass tune scope", e.code in (403,), str(e))
    try:
        a.call("debug.peek", "0x1000", "16"); check("tool without capability refused", False)
    except NeosError as e:
        check("tool without capability refused", e.code == 403)
    try:
        a.call("thread.kill", "0"); check("core thread untouchable", False)
    except NeosError as e:
        check("core thread untouchable", e.code in (403, 404), str(e))

    # --- develop an app inside NEOS: write sources, build a package, install it, read it back
    a.write_file("/src/demo/README.txt", b"demo app written by an agent\nline two\n")
    a.write_file("/src/demo/app.conf", b"quantum=7\nname=demo\n")
    b = a.call("pkg.build", "demo", "0.1", "/src/demo")
    check("package built from agent-written sources", b["files"] == 2 and len(b["sha256"]) == 64)
    i = a.call("pkg.install_file", "demo", "0.1", b["package"])
    check("package installed", i["root"] == "/opt/demo")
    check("installed file matches what the agent wrote", a.read_file("/opt/demo/README.txt") == b"demo app written by an agent\nline two\n")
    try:
        a.call("pkg.build", "other", "1", "/src/demo"); check("package name scope enforced", False)
    except NeosError as e:
        check("package name scope enforced", e.code == 403)

    # --- human-in-the-loop for irreversible actions
    res = {}
    def rm():
        try:
            res["r"] = a.call("fs.rm", "/src/demo/app.conf")
        except NeosError as e:
            res["e"] = e
    mark = len(text())
    t = threading.Thread(target=rm); t.start()
    wait_for("agent approve", 10, mark)
    pend = op("agent pending")
    check("pending request visible to the operator", "fs.rm" in pend and "coder" in pend)
    op("agent approve 1")
    t.join(10)
    check("approved irreversible action ran", "r" in res and res["r"]["removed"] == "/src/demo/app.conf")
    res.clear(); mark = len(text())
    t = threading.Thread(target=rm); t.start()
    wait_for("agent approve", 10, mark)
    idm = re.findall(r"agent deny (\d+)", text()[mark:])[-1]
    op(f"agent deny {idm}")
    t.join(10)
    check("denied action refused", "e" in res and res["e"].code == 403)

    # --- rollback of the agent's own changes
    before = a.call("tune.get", "sched.quantum")["value"]
    a.call("tune.set", "sched.quantum", "12")
    a.call("rollback", "1")
    check("agent rollback restores previous value", a.call("tune.get", "sched.quantum")["value"] == before)

    # --- experiment: apply, measure, keep only if better
    r = a.call("experiment.run", "rt.cost", "200", "timer.hz=2000")
    now = a.call("tune.get", "timer.hz")["value"]
    check("experiment returns baseline/trial and is consistent", (r["kept"] and now == "2000") or (not r["kept"] and now == "1000"), str(r))
    try:
        a.call("experiment.run", "rt.cost", "200", "timer.hz=9000"); check("experiment out of range refused", False)
    except NeosError as e:
        check("experiment out of range refused", e.code == 403)

    # --- freeze / thaw
    op("agent freeze")
    try:
        a.call("sys.info"); check("frozen agents are stopped", False)
    except NeosError as e:
        check("frozen agents are stopped", e.code == 423)
    op("agent thaw")
    check("thaw restores access", a.call("sys.info")["os"] == "NEOS")

    # --- rate limit
    op("tune set agent.rate_per_min 4")
    time.sleep(1.2)
    codes = []
    for _ in range(8):
        try:
            a.call("sys.info"); codes.append(200)
        except NeosError as e:
            codes.append(e.code)
    check("rate limit enforced", 429 in codes, str(codes))
    op("tune set agent.rate_per_min 600"); time.sleep(1.5)

    # --- audit chain
    out = op("agent log verify")
    check("audit hash chain intact", "intact" in out, out)
    tail = a.call("audit.tail", "5")
    check("audit shows the agent's actions", any(x["agent"] == "coder" for x in tail))

    # --- native agent: the tuner
    op("agent add tuner native")
    op("agent grant tuner observe experiment tune:timer.hz[250..4000],sched.quantum[2..20],net.poll_ms[1..5],ui.refresh_ms[250..2000],console.batch[128..2048]")
    mark = len(text())
    op("agent tuner run")
    wait_for("[tuner] done", 120, mark)
    log = text()[mark:]
    decisions = re.findall(r"\[tuner\] (\S+)\s+\S+ -> \S+\s+rt\.cost (-?\d+) -> (-?\d+)\s+(KEPT|reverted)", log)
    check("native tuner ran experiments through the gate", len(decisions) >= 5, str(len(decisions)))
    check("tuner decisions are consistent (kept only when better)", all((k == "KEPT") == (int(t) * 100 < int(b) * 95) for _, b, t, k in decisions))
    check("tuner stayed in its scope (no refusals)", "refused" not in log)
    out = op("agent log 40")
    check("tuner actions are in the audit log", out.count("experiment.run") >= 5)
    check("tuner left settings within its granted ranges", 250 <= int(re.search(r"timer.hz = (\d+)", op("tune get timer.hz")).group(1)) <= 4000)
    print("\n".join("      " + l for l in log.splitlines() if "[tuner]" in l))
finally:
    try:
        q.stdin.write(b"poweroff\n"); q.stdin.flush()
    except Exception:
        pass
    time.sleep(0.5)
    q.kill()
print("AGENT TEST " + ("FAILED: " + ", ".join(fails) if fails else "OK"))
sys.exit(1 if fails else 0)
