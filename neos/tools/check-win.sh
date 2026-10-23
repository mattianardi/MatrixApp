#!/bin/sh
# Unmodified Windows (PE64) programs on NEOS, and Windows <-> Linux cooperation through files, pipes and the variable bus.
cd "$(dirname "$0")/.." || exit 1
out=$(NEOS_BOOT_WAIT=10 NEOS_STEP=5 NEOS_TIMEOUT=260 tools/shell-session.sh \
  'run /bin/winext.exe' \
  'run /bin/wintest.exe one "two words"' \
  'cat /mnt/c/shared/win.txt' \
  'cat /mnt/c/shared/crt.txt' \
  'run /bin/wincoop.exe' \
  'run /bin/busybox sh -c "echo made-by-linux > /mnt/c/shared/lin.txt"' \
  'set NEOS_X=C:\data\x' \
  'run /bin/wintest.exe' \
  "run /bin/busybox sh -c 'echo lin=\$NEOS_X'")
fail=0
check() { printf '%s\n' "$out" | grep -qF -- "$1" || { echo "MISSING: $1"; fail=1; }; }
check 'EXT reg read=0 type=1 value=hello registry'
check 'EXT reg dword=1234'
check 'EXT reg deleted=yes'
check 'EXT reg system=0 10.0'
check "EXT sock connect=0 recv=13 'WINSOCK WORKS'"
check 'EXT sock refused=-1 err=10061'
check 'EXT condvar value=42'
check 'EXT once runs=1'
check 'EXT com create=REGDB_E_CLASSNOTREG'
check 'EXT done'
check 'WIN argc=3 argv0=\\neos\root\bin\wintest.exe'
check 'WIN arg2=two words'
check 'WIN fmt  3.14|42    |0000beef|str|z|1234567890123|99|wide'
check 'WIN read 13: from-windows'
check 'WIN threads counter=4 exit=7 tls=5'
check 'WIN found 2 txt files'
check 'WIN done'
check 'WIN env NEOS_X=C:\data\x'
check 'lin=/mnt/c/data/x'
check 'exited with code 3'
check 'from-windows'
check 'crt line 9'
check 'COOP child output:'
check 'CHILD got: HELLO NEOS'
check 'CHILD got: SECOND LINE'
check 'COOP child exit code 5'
check 'WIN found 3 txt files'
printf '%s\n' "$out" | grep -qF 'unimplemented Windows API' && { echo "UNIMPLEMENTED API hit"; fail=1; }
printf '%s\n' "$out" | grep -qF 'killed:' && { echo "UNEXPECTED process kill"; fail=1; }
[ $fail -eq 0 ] && echo "WIN TEST OK" || { printf '%s\n' "$out" | tail -70; echo "WIN TEST FAILED"; exit 1; }
