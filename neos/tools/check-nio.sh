#!/bin/sh
# Nio on NEOS: the language test-suite (expected output written by hand), the Linux/Windows path+env bridge,
# and the capability-gated system tools reached from a script.
cd "$(dirname "$0")/.." || exit 1
out=$(NEOS_BOOT_WAIT=12 NEOS_STEP=6 NEOS_TIMEOUT=240 tools/shell-session.sh \
  'export NIO_TEST_VAR=hello-nio' \
  "run /bin/busybox sh -c 'for t in basics collections functions errors json_paths os; do /bin/nio /lib/nio/tests/\$t.nio > /tmp/\$t.got 2>&1; if cmp -s /tmp/\$t.got /lib/nio/tests/\$t.out; then echo PASS-\$t; else echo FAIL-\$t; fi; done'" \
  'export PATH=/usr/bin:/mnt/c/tools' \
  "run /bin/nio -e 'println(env.lin(\"PATH\")); println(env.win(\"PATH\")); println(path.win(\"/mnt/c/Users/neos\"))'" \
  "run /bin/nio -e 'let ts = sys.threads() |> filter(.prio < 7) |> map(.name); println(\"threads:\", len(ts) > 3, \"shell\" in ts); println(sys.mem().phys_free_kib > 1000)'" \
  "run /bin/nio -e 'try { sys.tune(\"sched.quantum\", 7) } catch e { println(\"denied:\", e) }'" \
  "run -g tune:sched.quantum[2..20] /bin/nio -e 'println(sys.tune(\"sched.quantum\", 6).applied); println(sys.tune(\"sched.quantum\")); try { sys.tune(\"heap.size_mb\", 9) } catch e { println(\"out of scope:\", e) }'" \
  "run /bin/nio -e 'println(sys.whoami().caps)'")
fail=0
check() { printf '%s\n' "$out" | grep -qF -- "$1" || { echo "MISSING: $1"; fail=1; }; }
for t in basics collections functions errors json_paths os; do check "PASS-$t"; done
check '/usr/bin:/mnt/c/tools'
check '\\neos\root\usr\bin;C:\tools'
check 'C:\Users\neos'
check 'threads: true true'
check 'true'
check 'denied: tool failed (403): missing capability'
check 'out of scope: tool failed (403): outside this agent'
check '{observe: "(no scope)"}'
printf '%s\n' "$out" | grep -q '^FAIL-' && { echo "A language test failed"; fail=1; }
[ $fail -eq 0 ] && echo "NIO TEST OK" || { printf '%s\n' "$out" | tail -40; echo "NIO TEST FAILED"; exit 1; }
