"""Client for the NEOS agent channel (NAP/1).

Connects to the COM2 link of a NEOS machine (QEMU: -serial unix:PATH,server,nowait, or a TCP/pty
bridge on real hardware). Usable directly from an LLM tool loop:

    a = NeosAgent.unix("build/agent.sock"); a.auth("coder", TOKEN)
    schema = a.tools()                        # JSON tool schemas for the model
    a.call("tune.set", "sched.quantum", "5")  # -> dict, or raises NeosError(code, message)
"""
import base64, json, shlex, socket, time


class NeosError(Exception):
    def __init__(self, code, message):
        super().__init__(f"{code} {message}")
        self.code, self.message = code, message


class NeosAgent:
    def __init__(self, sock):
        self.sock, self.buf, self.seq = sock, b"", 0

    @classmethod
    def unix(cls, path, timeout=30):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        s.connect(path)
        a = cls(s)
        a._send("HELLO")
        a._line()
        return a

    def _send(self, text):
        self.sock.sendall(text.encode() + b"\n")

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("agent channel closed")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode(errors="replace").rstrip("\r")

    def auth(self, name, token):
        self._send(f"AUTH {name} {token}")
        r = self._line()
        if not r.startswith("OK"):
            raise NeosError(401, r)

    def call(self, tool, *args, raw=False):
        self.seq += 1
        self._send(f"CALL {self.seq} {tool} " + " ".join(shlex.quote(str(x)) if not str(x).isalnum() else str(x) for x in args))
        r = self._line()
        parts = r.split(" ", 3)
        if parts[0] == "OK":
            return parts[2] if raw else json.loads(r.split(" ", 2)[2])
        raise NeosError(int(parts[2]), parts[3] if len(parts) > 3 else "")

    def tools(self):
        return self.call("tools")

    # conveniences for app development ---------------------------------------------------------
    def write_file(self, path, data: bytes, chunk=1024):
        mode = None
        for i in range(0, max(len(data), 1), chunk):
            self.call("fs.write", path, base64.b64encode(data[i:i + chunk]).decode(), *([mode] if mode else []))
            mode = "append"

    def read_file(self, path):
        out, off = b"", 0
        while True:
            r = self.call("fs.read", path, off, 768)
            out += base64.b64decode(r["data_b64"])
            off += r["len"]
            if off >= r["size"]:
                return out
