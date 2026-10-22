#!/bin/sh
# Scripted shell session with assertions on the output. Exit 0 = all good.
cd "$(dirname "$0")/.." || exit 1
out=$(tools/shell-session.sh \
  'export PATH=/usr/bin:/mnt/c/tools' \
  'set Path=C:\bin;D:\apps' \
  'echo L=$PATH W=%PATH% X=${PATH@win}' \
  'set USERPROFILE=D:\home\me' \
  'echo $HOME' \
  'tune set sched.quantum 7' \
  'echo q=${NEOS.sched.quantum}' \
  'tune set heap.size_mb 16' \
  'ps | where prio<5 | count' \
  'sys var.get PATH win' \
  'path win /mnt/c/Users/x' \
  'path lin D:\data\f.txt' \
  'echo unknown=%NOPE% def=${NOPE:-fallback}')
fail=0
check() { printf '%s\n' "$out" | grep -qF -- "$1" || { echo "MISSING: $1"; fail=1; }; }
check 'L=/mnt/c/bin:/mnt/d/apps W=C:\bin;D:\apps X=C:\bin;D:\apps'
check '/mnt/d/home/me'
check 'q=7'
check 'takes effect at next boot'
check 'C:\bin;D:\apps'
check 'C:\Users\x'
check '/mnt/d/data/f.txt'
check 'unknown=%NOPE% def=fallback'
[ $fail -eq 0 ] && echo "SHELL TEST OK" || { printf '%s\n' "$out" | tail -40; echo "SHELL TEST FAILED"; exit 1; }
