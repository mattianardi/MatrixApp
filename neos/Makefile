CC      := gcc
LD      := ld
NASM    := nasm
BUILD   := build
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-pic -fno-pie \
           -fno-stack-protector -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -Ikernel
LDFLAGS := -T kernel/linker.ld -nostdlib -static -z max-page-size=0x1000

CSRC := $(wildcard kernel/*.c)
OBJS := $(BUILD)/boot.o $(BUILD)/isr.o $(patsubst kernel/%.c,$(BUILD)/%.o,$(CSRC))

AGENT_CHAN ?= -serial unix:$(BUILD)/agent.sock,server,nowait
NIC ?= -nic user,model=e1000
DISK ?=
KARGS ?=
QEMU = qemu-system-x86_64 -kernel $(BUILD)/matrix32.elf -initrd $(BUILD)/initrd.npk -m 512 -display none -append "$(KARGS)" \
        -cpu max -serial stdio $(AGENT_CHAN) -no-reboot $(NIC) $(DISK) -device isa-debug-exit,iobase=0xf4,iosize=0x04

USER_CFLAGS := -O2 -Wall -ffreestanding -fno-stack-protector -nostdlib -static -mno-avx -fno-asynchronous-unwind-tables \
               -Wl,--build-id=none -Wl,-z,max-page-size=4096
USER_PROGS  := hello hello-pie crash spin
LINUX_BINS  := hello_glibc fileio forkpipe threads hello_musl hello_rs hello_go netcheck webserver sigtest
LINUX_CC    := gcc -O2 -static -fno-pie -no-pie

all: $(BUILD)/matrix32.elf $(BUILD)/initrd.npk

# user programs: hello is linked at 0x400000 (the classic non-PIE address), hello-pie is a static PIE (relocated by the loader)
$(BUILD)/user/hello: user/hello.c user/neos.h kernel/neos_api.h | $(BUILD)
	@mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) -Wl,-Ttext-segment=0x400000 -o $@ $<
$(BUILD)/user/hello-pie: user/hello.c user/neos.h kernel/neos_api.h | $(BUILD)
	@mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) -fPIE -static-pie -o $@ $<
$(BUILD)/user/crash: user/crash.c user/neos.h kernel/neos_api.h | $(BUILD)
	@mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) -Wl,-Ttext-segment=0x400000 -o $@ $<
$(BUILD)/user/spin: user/spin.c user/neos.h kernel/neos_api.h | $(BUILD)
	@mkdir -p $(BUILD)/user
	$(CC) $(USER_CFLAGS) -Wl,-Ttext-segment=0x400000 -o $@ $<

$(BUILD)/linux/%: linuxtests/%.c | $(BUILD)
	@mkdir -p $(BUILD)/linux
	$(LINUX_CC) -o $@ $<
$(BUILD)/linux/hello_musl: linuxtests/hello_musl.c | $(BUILD)
	@mkdir -p $(BUILD)/linux
	musl-gcc -O2 -static -o $@ $<
$(BUILD)/linux/hello_rs: linuxtests/hello_rs.rs | $(BUILD)
	@mkdir -p $(BUILD)/linux
	rustc -O --target x86_64-unknown-linux-musl -C target-feature=+crt-static $< -o $@
$(BUILD)/linux/hello_go: linuxtests/hello_go.go | $(BUILD)
	@mkdir -p $(BUILD)/linux
	CGO_ENABLED=0 go build -o $@ $<
$(BUILD)/linux/webserver: linuxtests/webserver.go | $(BUILD)
	@mkdir -p $(BUILD)/linux
	CGO_ENABLED=0 go build -o $@ $<
$(BUILD)/linux/nio: lang/nio.c | $(BUILD)
	@mkdir -p $(BUILD)/linux
	gcc -O2 -static -DNIO_NEOS -Wno-misleading-indentation -o $@ $< -lm
NIO_TESTS := $(wildcard lang/tests/*.nio lang/tests/*.out)
WINRT_SRC := $(wildcard winrt/*.c winrt/*.h)
$(BUILD)/linux/winrt: $(WINRT_SRC) | $(BUILD)
	@mkdir -p $(BUILD)/linux
	gcc -O2 -static -Wno-attributes -o $@ winrt/*.c -lm -lpthread
WIN_EXES := wintest winchild wincoop
$(BUILD)/win/%.exe: winrt/tests/%.c | $(BUILD)
	@mkdir -p $(BUILD)/win
	x86_64-w64-mingw32-gcc -O2 -o $@ $<
$(BUILD)/linux/neosctl: userland/neosctl.c | $(BUILD)
	@mkdir -p $(BUILD)/linux
	gcc -O2 -static -o $@ $<
USERLAND := $(wildcard userland/lib/neos/pkg/*)
$(BUILD)/linux/busybox: | $(BUILD)
	@mkdir -p $(BUILD)/linux
	cp /usr/bin/busybox $@

$(BUILD)/initrd.npk: $(addprefix $(BUILD)/user/,$(USER_PROGS)) $(addprefix $(BUILD)/linux/,$(LINUX_BINS)) $(BUILD)/linux/busybox $(BUILD)/linux/nio $(BUILD)/linux/winrt $(BUILD)/linux/neosctl $(USERLAND) $(addprefix $(BUILD)/win/,$(addsuffix .exe,$(WIN_EXES))) $(NIO_TESTS) $(wildcard webapps/*) tools/mkinitrd.py
	python3 tools/mkinitrd.py $@ $(foreach p,$(USER_PROGS),bin/$(p)=$(BUILD)/user/$(p)) $(foreach p,$(LINUX_BINS),bin/$(p)=$(BUILD)/linux/$(p)) bin/busybox=$(BUILD)/linux/busybox bin/nio=$(BUILD)/linux/nio bin/winrt=$(BUILD)/linux/winrt bin/neosctl=$(BUILD)/linux/neosctl $(foreach f,$(USERLAND),lib/neos/pkg/$(notdir $(f))=$(f)) $(foreach w,$(WIN_EXES),bin/$(w).exe=$(BUILD)/win/$(w).exe) $(foreach f,$(NIO_TESTS),lib/nio/tests/$(notdir $(f))=$(f)) $(foreach f,$(wildcard webapps/*),www/$(notdir $(f))=$(f)) etc/motd=README.md

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/%.o: kernel/%.c $(wildcard kernel/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/matrix.elf: $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# QEMU's multiboot loader only accepts 32-bit ELF images.
$(BUILD)/matrix32.elf: $(BUILD)/matrix.elf
	objcopy -O elf32-i386 $< $@

$(BUILD):
	mkdir -p $@

$(BUILD)/neos.disk: | $(BUILD)
	truncate -s 64M $@

run: DISK = -drive file=$(BUILD)/neos.disk,format=raw,if=ide
run: all $(BUILD)/neos.disk
	$(QEMU)

# Graphical session in a QEMU window (keyboard + mouse go to the NEOS workspace; the serial console stays in this terminal).
gui: DISK = -drive file=$(BUILD)/neos.disk,format=raw,if=ide
gui: all $(BUILD)/neos.disk
	$(subst -display none,-display default -vga std,$(QEMU))

# Bootable ISO (GRUB + multiboot): runs in QEMU, VirtualBox, VMware, or from a USB stick on a PC with BIOS boot.
$(BUILD)/neos.iso: all
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp $(BUILD)/matrix32.elf $(BUILD)/initrd.npk $(BUILD)/iso/boot/
	printf 'set timeout=2\nset gfxpayload=1024x768x32\nmenuentry "NEOS" {\n  multiboot /boot/matrix32.elf\n  module /boot/initrd.npk\n}\n' > $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null
iso: $(BUILD)/neos.iso
run-iso: $(BUILD)/neos.iso $(BUILD)/neos.disk
	qemu-system-x86_64 -cdrom $(BUILD)/neos.iso -boot d -m 512 -cpu max -vga std -serial stdio -nic user,model=e1000 -drive file=$(BUILD)/neos.disk,format=raw,if=ide

# Exit code 33 == kernel self-test passed (isa-debug-exit 0x10 -> (0x10<<1)|1).
test: KARGS := neos.boot.test=1
test: all
	@timeout 30 $(QEMU); rc=$$?; if [ $$rc -eq 33 ]; then echo "TEST OK"; else echo "TEST FAILED (rc=$$rc)"; exit 1; fi
	@$(MAKE) --no-print-directory shelltest
	@$(MAKE) --no-print-directory nettest
	@$(MAKE) --no-print-directory usertest
	@$(MAKE) --no-print-directory linuxtest
	@$(MAKE) --no-print-directory wintest
	@$(MAKE) --no-print-directory niotest
	@$(MAKE) --no-print-directory aihtest
	@$(MAKE) --no-print-directory agenttest
	@$(MAKE) --no-print-directory guitest
	@$(MAKE) --no-print-directory browsertest
	@$(MAKE) --no-print-directory netappstest
	@$(MAKE) --no-print-directory disktest

shelltest: all
	@tools/check-shell.sh

nettest: all
	@tools/check-net.sh

linuxtest: all
	@tools/check-linux.sh

usertest: all
	@tools/check-user.sh

wintest: all
	@tools/check-win.sh

niotest: all
	@tools/check-nio.sh

aihtest: all
	@python3 tools/check-aih.py | tail -1

netappstest: all
	@python3 tools/check-netapps.py | tail -1

browsertest: all
	@python3 tools/check-browser.py | tail -1

disktest: all
	@tools/check-disk.sh

guitest: all
	@python3 tools/check-gui.py | tail -1

agenttest: all
	@python3 tools/check-agent.py | tail -1

clean:
	rm -rf $(BUILD)

.PHONY: all run gui iso run-iso test shelltest nettest usertest linuxtest wintest niotest aihtest agenttest guitest browsertest netappstest disktest clean
