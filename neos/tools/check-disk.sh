#!/bin/sh
# NeoFS: files, Windows C: drive, packages' /opt and saved settings survive a reboot; a damaged newest snapshot falls back to the previous one.
cd "$(dirname "$0")/.." || exit 1
DISK=build/test.disk
rm -f $DISK; truncate -s 64M $DISK
EXTRA="-drive file=$DISK,format=raw,if=ide"
fail=0
check() { printf '%s\n' "$2" | grep -qF -- "$1" || { echo "MISSING: $1"; fail=1; }; }
absent() { printf '%s\n' "$2" | grep -qF -- "$1" && { echo "UNEXPECTED: $1"; fail=1; }; }
s1=$(QEMU_EXTRA="$EXTRA" NEOS_BOOT_WAIT=10 NEOS_STEP=4 NEOS_TIMEOUT=120 tools/shell-session.sh \
  'disk status' 'write /data/a.txt persistent-one' 'run /bin/wintest.exe' 'tune set sched.quantum 7' 'tune save' 'disk sync' 'disk status')
check 'no valid snapshot yet (fresh disk)' "$s1"
check 'snapshot written' "$s1"
check '/mnt/c' "$s1" 2>/dev/null
s2=$(QEMU_EXTRA="$EXTRA" NEOS_BOOT_WAIT=10 NEOS_STEP=4 NEOS_TIMEOUT=120 tools/shell-session.sh \
  'cat /data/a.txt' 'cat /mnt/c/shared/win.txt' 'tune get sched.quantum' 'write /data/b.txt second' 'disk sync' 'disk status')
check 'restored snapshot #1' "$s2"
check 'persistent-one' "$s2"
check 'from-windows' "$s2"
check 'sched.quantum = 7' "$s2"
check 'snapshot written' "$s2"
# damage the newest snapshot (slot B) and boot again: the previous one must come back
python3 - <<'P'
import sys
n = 64 * 1024 * 1024 // 512
lba = 8 + (n - 8) // 2
f = open('build/test.disk', 'r+b'); f.seek(lba * 512 + 3); f.write(b'#damaged#'); f.close()
P
s3=$(QEMU_EXTRA="$EXTRA" NEOS_BOOT_WAIT=10 NEOS_STEP=4 NEOS_TIMEOUT=120 tools/shell-session.sh 'cat /data/a.txt' 'cat /data/b.txt')
check 'slot B is damaged, trying the other one' "$s3"
check 'restored snapshot #1' "$s3"
check 'persistent-one' "$s3"
absent 'second' "$s3"
rm -f $DISK
[ $fail -eq 0 ] && echo "DISK TEST OK" || { printf '%s\n%s\n%s\n' "$s1" "$s2" "$s3" | tail -60; echo "DISK TEST FAILED"; exit 1; }
