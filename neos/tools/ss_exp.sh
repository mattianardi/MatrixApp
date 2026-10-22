#!/bin/sh
# Drive the NEOS shell over the serial port: tools/shell-session.sh 'cmd1' 'cmd2' ...
# Output is the raw serial stream (ANSI colours stripped unless NEOS_RAW=1).
cd "$(dirname "$0")/.." || exit 1
QEMU_EXTRA=${QEMU_EXTRA:-}
{
  sleep ${NEOS_BOOT_WAIT:-10}
  for c in "$@"; do printf '%s\n' "$c"; sleep ${NEOS_STEP:-0.4}; done
  printf 'poweroff\n'; sleep 1
} | timeout ${NEOS_TIMEOUT:-60} qemu-system-x86_64 -kernel build/matrix32.elf -initrd build/exp.npk -m 512 -display none \
      -cpu max -serial stdio ${AGENT_CHAN:--serial null} -no-reboot -append "${KARGS:-}" $QEMU_EXTRA 2>&1 \
  | { if [ -n "$NEOS_RAW" ]; then cat; else sed 's/\x1b\[[0-9;]*[A-Za-z]//g'; fi; }
