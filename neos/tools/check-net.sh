#!/bin/sh
# Network + package manager test against local HTTP servers (QEMU user-mode networking, host = 10.0.2.2).
cd "$(dirname "$0")/.." || exit 1
T=build/nettest; rm -rf $T; mkdir -p $T/good $T/bad
python3 tools/mkrepo.py repo-src/packages $T/good >/dev/null
cp $T/good/* $T/bad/
python3 - <<PY
p="$T/bad/hello-1.0.npk"; b=bytearray(open(p,"rb").read()); b[12]^=1; open(p,"wb").write(b)   # same size, one bit flipped
PY
head -c 300000 /dev/urandom > $T/good/big.bin
WANT=$(sha256sum $T/good/big.bin | cut -c1-64)
P1=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
P2=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
(cd $T/good && exec python3 -m http.server $P1 --bind 127.0.0.1 >/dev/null 2>&1) & S1=$!
(cd $T/bad  && exec python3 -m http.server $P2 --bind 127.0.0.1 >/dev/null 2>&1) & S2=$!
trap 'kill $S1 $S2 2>/dev/null' EXIT
sleep 1
out=$(QEMU_EXTRA='-nic user,model=e1000' NEOS_STEP=2 NEOS_BOOT_WAIT=3 NEOS_TIMEOUT=90 tools/shell-session.sh \
  'net' \
  'ping 10.0.2.2 2' \
  "tune set pkg.repo http://10.0.2.2:$P1" \
  'pkg update' \
  'tune set pkg.allow_insecure on' \
  'pkg update' \
  'pkg install hello' \
  'cat \\neos\root\opt\hello\README.txt' \
  "tune set pkg.repo http://10.0.2.2:$P2" \
  'pkg update' \
  'pkg install hello' \
  "http http://10.0.2.2:$P1/big.bin /tmp/big.bin" \
  'sha256 /tmp/big.bin' \
  'pkg remove hello' \
  'ls /opt')
fail=0
check() { printf '%s\n' "$out" | grep -qF -- "$1" || { echo "MISSING: $1"; fail=1; }; }
check '10.0.2.15'
check '2 sent, 0 lost'
check 'plain-HTTP repository refused'
check '2 packages'
check 'installed hello 1.0: 2 files in /opt/hello (sha256 verified)'
check 'Hello from a NEOS package!'
check 'SHA-256 mismatch: package corrupted or tampered'
check "$WANT"
check 'removed hello (2 files)'
[ $fail -eq 0 ] && echo "NET TEST OK" || { printf '%s\n' "$out" | tail -60; echo "NET TEST FAILED"; exit 1; }
