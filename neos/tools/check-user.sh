#!/bin/sh
# User mode: ELF (fixed address and PIE), isolation, capabilities, interop environment, kill, no memory leaks.
cd "$(dirname "$0")/.." || exit 1
out=$(NEOS_BOOT_WAIT=10 NEOS_STEP=2.2 NEOS_TIMEOUT=160 tools/shell-session.sh \
  'echo FREE0=${NEOS.mem.free_kib}' \
  'run /bin/hello one "two words"' \
  'run /bin/hello-pie' \
  'run -w win /bin/hello' \
  'run /bin/crash badbuf' \
  'run /bin/crash kmem' \
  'run /bin/crash Kread' \
  'run /bin/crash null' \
  'run /bin/crash cli' \
  'run /bin/crash io' \
  'run /bin/crash exec' \
  'run /bin/crash div' \
  'tune set proc.max_pages 4096' \
  'run /bin/crash mem' \
  'tune set proc.max_pages 65536' \
  'run -g tune:sched.quantum[2..20] /bin/hello' \
  'run -b /bin/spin' \
  'proc | where state=running | cols name state' \
  'pkill 14' 'sleep 300' \
  'run /bin/crash null' \
  'echo FREE1=${NEOS.mem.free_kib}' \
  'selftest')
fail=0
check() { printf '%s\n' "$out" | grep -qF -- "$1" || { echo "MISSING: $1"; fail=1; }; }
check "args: '/bin/hello' 'one' 'two words'"
check 'hello from ring 3'
check 'main at 0x401000'
check 'main at 0x555555555000'
check 'float check: 377'
check 'mmap ok: AZ'
check '[pid 1] exited with code 42'
check 'world=linux'
check 'HOME=/mnt/c/Users/neos'
check 'world=windows'
check 'HOME=C:\Users\neos'
check ';C:\Windows\System32'
check 'write(kernel address) -> -14'
check 'tool(kernel cmd pointer) -> -14'
check 'unknown syscall -> -38'
check 'address 0xffff800000000000 (protection violation, write)'
check 'address 0xffffffff80100000 (protection violation, read)'
check 'address 0x0000000000000000 (not mapped, read)'
check 'general protection fault'
check 'divide error'
check 'instruction fetch'
check 'mmap refused at'
check 'tool tune.set rc=403 missing capability'
check '"new":"7","applied":true'
check 'spin  running'
check 'SELFTEST PASS'
a=$(printf '%s\n' "$out" | grep -o 'FREE0=[0-9][0-9]*' | head -1 | cut -d= -f2)
b=$(printf '%s\n' "$out" | grep -o 'FREE1=[0-9][0-9]*' | head -1 | cut -d= -f2)
[ -n "$a" ] && [ "$a" = "$b" ] || { echo "MEMORY LEAK or bad sample: before=$a after=$b KiB"; fail=1; }
[ $fail -eq 0 ] && echo "USER TEST OK (free memory $a KiB before and after 13 processes)" || { printf '%s\n' "$out" | tail -70; echo "USER TEST FAILED"; exit 1; }
