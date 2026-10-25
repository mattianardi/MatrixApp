#!/bin/sh
# Start NEOS headless with its console on a TCP port, to be driven by hand (tools/ser.py): tools/neos-vm.sh [mem MiB] [serial port] [initrd modules, comma separated]
cd "$(dirname "$0")/.." || exit 1
MEM=${1:-1024}; PORT=${2:-4561}; INITRD=${3:-build/initrd.npk}
exec qemu-system-x86_64 -kernel build/matrix32.elf -initrd "$INITRD" -m "$MEM" -display none -cpu max -vga std -no-reboot \
  -serial "tcp:127.0.0.1:$PORT,server=on,wait=off" -nic "user,model=e1000,hostfwd=tcp::${4:-18810}-:8800"
