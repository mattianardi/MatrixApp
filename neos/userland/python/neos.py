"""neos - Python access to the NEOS kernel tools: the same capability-gated tools AI agents use.

    import neos
    neos.tune("sched.quantum")            # read a setting
    neos.tune("sched.quantum", 5)         # change it (live)
    neos.threads()                        # -> list of dicts
    neos.tool("pkg.list")                 # any tool by name; returns the parsed JSON result

A program starts with the `observe` capability only; the operator grants more (`run -g tune:sched.quantum[2..20] python3 script.py`).
Errors raise neos.NeosError with the tool's message and code."""
import json, subprocess

class NeosError(Exception):
    def __init__(self, code, message): super().__init__(f"{message} (code {code})"); self.code = code; self.message = message

def tool(name, *args):
    p = subprocess.run(["/bin/neosctl", name, *[str(a) for a in args]], capture_output=True, text=True)
    text = (p.stdout or p.stderr).strip()
    if p.returncode: raise NeosError(p.returncode, text)
    try: return json.loads(text)
    except ValueError: return text

def tune(name, value=None):
    return tool("tune.get", name) if value is None else tool("tune.set", name, value)

def threads(): return tool("threads")
def info(): return tool("sys.info")
