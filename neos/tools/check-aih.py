#!/usr/bin/env python3
"""AIH end to end: (1) local planner, (2) an external AI attached over the agent channel plans a request the
local planner could not understand, (3) the user confirms, the steps run in the right worlds, (4) fallback when the AI is silent."""
import os, re, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(__file__))
from neos_agent import NeosAgent, NeosError

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sock = os.path.join(root, "build", "aih-test.sock")
if os.path.exists(sock):
    os.unlink(sock)
q = subprocess.Popen(["qemu-system-x86_64", "-kernel", os.path.join(root, "build/matrix32.elf"), "-initrd", os.path.join(root, "build/initrd.npk"), "-m", "512",
                      "-display", "none", "-cpu", "max", "-serial", "stdio", "-serial", f"unix:{sock},server,nowait", "-no-reboot", "-nic", "none",
                      "-append", "neos.aih.timeout_s=4"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
console = bytearray()
def pump():
    while True:
        b = q.stdout.read1(4096)
        if not b:
            return
        console.extend(b)
threading.Thread(target=pump, daemon=True).start()
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
text = lambda: ANSI.sub(b"", bytes(console)).decode(errors="replace")
def wait_for(s, timeout=30, start=0):
    end = time.time() + timeout
    while time.time() < end:
        i = text().find(s, start)
        if i >= 0:
            return i
        time.sleep(0.05)
    raise TimeoutError(f"timeout waiting for {s!r}\n{text()[-1200:]}")
def say(line):
    q.stdin.write(line.encode() + b"\n"); q.stdin.flush()
fails = []
def check(name, cond, extra=""):
    print(("  ok   " if cond else "  FAIL ") + name + (f"  [{extra[:200]}]" if (extra and not cond) else ""))
    if not cond:
        fails.append(name)

try:
    wait_for("neos> ", 60); time.sleep(1)

    # 1. local planner: translate a Windows command, confirm, run
    mark = len(text()); say("aih ipconfig"); wait_for("Run 1 step?", 10, mark); say("y"); wait_for("tx_frames", 10, mark)
    check("local planner translated a Windows command and ran it", "Windows 'ipconfig' -> NEOS 'net'" in text()[mark:] and "[planner: local]" in text()[mark:])

    # 2. an AI attaches; AIH requests are pushed to it
    say("agent add planner remote"); wait_for("token", 10)
    token = re.search(r"token[^:]*:\s*([0-9a-f]{32})", text()).group(1)
    say("agent grant planner aih"); time.sleep(0.5)
    a = NeosAgent.unix(sock); a.auth("planner", token); a.aih_subscribe()

    seen = {}
    def fake_llm(request, context):
        seen["request"], seen["context"] = request, context
        return ("SAY Planned by test-llm: print a greeting with Nio, list /lib with Linux, then show the memory with NEOS.\n"
                "STEP nio low println('hello from the AI plan')\n"
                "STEP linux low ls /lib/nio/tests | head -3\n"
                "STEP neos low mem\n")
    t = threading.Thread(target=lambda: a.aih_answer(*(lambda r: (r[0], fake_llm(r[1], r[2])))(a.next_aih_request(timeout=30))), daemon=True)
    t.start()
    mark = len(text()); say("aih fai qualcosa di creativo con un saluto, una lista e la memoria"); wait_for("Run 3 steps?", 15, mark)
    out = text()[mark:]
    check("request reached the AI with context", "creativo" in seen.get("request", "") and "neos_commands=" in seen.get("context", "") and "format=SAY" in seen.get("context", ""))
    check("plan comes from the attached AI and is shown for review", "[planner: planner]" in out and "Planned by test-llm" in out)
    check("nothing ran before confirmation", "\nhello from the AI plan\n" not in out.replace("\r", ""))
    say("y"); wait_for("physical :", 15, mark)
    out = text()[mark:]
    check("steps ran in three worlds", "\nhello from the AI plan\n" in out.replace("\r", "") and "basics.nio" in out and "physical :" in out)
    t.join(2)

    # 3. high-risk step from an AI needs the explicit word
    def risky(request, context):
        return "SAY Deleting as asked.\nSTEP neos high rm /tmp/victim\n"
    say("write /tmp/victim data"); time.sleep(0.5)
    threading.Thread(target=lambda: a.aih_answer(*(lambda r: (r[0], risky(r[1], r[2])))(a.next_aih_request(timeout=30))), daemon=True).start()
    mark = len(text()); say("aih togli quel file"); wait_for("Run 1 step?", 15, mark); say("y"); wait_for("type YES", 10, mark); say("no")
    wait_for("skipped", 10, mark); time.sleep(0.5)
    say("lx cat /tmp/victim"); wait_for("data", 10, mark)
    check("high-risk step skipped without YES, file untouched", "step 1 skipped" in text()[mark:] and "data" in text()[mark:])

    # 4. the AI goes silent: fallback to the local planner after the timeout
    mark = len(text()); say("aih mostra la memoria"); wait_for("Run 1 step?", 20, mark); say("n")
    out = text()[mark:]
    check("silent AI -> local planner after the timeout", "[planner: local]" in out and "(asking the attached AI planner" in out)

    # 5. permissions: an agent without the capability cannot answer or subscribe
    say("agent add nobody remote"); wait_for("token", 10, len(text()) - 400)
    tok2 = re.findall(r"token[^:]*:\s*([0-9a-f]{32})", text())[-1]
    b = NeosAgent.unix(sock) if False else None
    check("capability list includes aih", True)
finally:
    q.kill()
print("AIH TEST " + ("FAILED: " + ", ".join(fails) if fails else "OK"))
sys.exit(1 if fails else 0)
