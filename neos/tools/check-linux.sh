#!/bin/sh
# Unmodified Linux programs on NEOS: glibc, musl, Rust, Go, BusyBox (shell, pipes), threads, files.
cd "$(dirname "$0")/.." || exit 1
want_go=$(cd build/lang 2>/dev/null && go run hello_go.go 2>/dev/null | grep -o 'total = [0-9]*')
out=$(NEOS_BOOT_WAIT=10 NEOS_STEP=6 NEOS_TIMEOUT=240 tools/shell-session.sh \
  'run /bin/hello_glibc a b' \
  'run /bin/hello_musl' \
  'run /bin/fileio' \
  'run /bin/forkpipe' \
  'run /bin/threads' \
  'run /bin/hello_rs x' \
  'run /bin/hello_go' \
  'run /bin/sigtest' \
  "run /bin/busybox sh -c 'echo from-sh; ls /bin | wc -l; echo \$((6*7)) | cat; true && echo and-ok'" \
  "run /bin/busybox sh -c 'echo b a c | tr \" \" \"\\n\" | sort | head -2'" \
  'run /bin/busybox uname -s' \
  'run /bin/busybox sh -c "exit 9"')
fail=0
check() { printf '%s\n' "$out" | grep -qF -- "$1" || { echo "MISSING: $1"; fail=1; }; }
check 'hello from an unmodified Linux binary (glibc, static) on Linux'
check "argv[0]='/bin/hello_glibc'"
check 'qsort: 1 3 5 7 9'
check 'malloc 5 MiB ok'
check 'clock: year 20'
check '[pid 1] exited with code 7'
check 'hello from a musl-libc static binary'
check 'cleanup: gone'
check 'proc: MemTotal:'
check 'parent got: message from the child through a pipe'
check 'child exited normally=1 code=3'
check 'pipeline done'
check '4 threads, counter=80000 (expected 80000)'
check 'hello from Rust on NEOS: 2 args, squares sum = 385'
check 'thread result: 500500'
check "hello from Go on NEOS, goroutines $want_go"
check 'SIG raise: handler ran, value 10001'
check 'SIG blocked: pending, handler not run yet (0)'
check 'SIG unblocked: handler ran (1)'
check 'SIG alarm interrupted sleep: r=-1 alarms=1'
check 'SIG child exit: status 3 sigchld=1'
check 'SIG segv caught and recovered: code=7 hits=1'
check 'SIG fpu state preserved: 5.468'
check 'from-sh'
check '42'
check 'and-ok'
check 'exited with code 9'
printf '%s\n' "$out" | grep -qF 'killed:' && { echo "UNEXPECTED process kill"; fail=1; }
[ $fail -eq 0 ] && echo "LINUX TEST OK" || { printf '%s\n' "$out" | tail -60; echo "LINUX TEST FAILED"; exit 1; }
