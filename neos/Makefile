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
KARGS ?=
QEMU = qemu-system-x86_64 -kernel $(BUILD)/matrix32.elf -initrd $(BUILD)/initrd.npk -m 128 -display none -append "$(KARGS)" \
        -cpu max -serial stdio $(AGENT_CHAN) -no-reboot $(NIC) -device isa-debug-exit,iobase=0xf4,iosize=0x04

USER_CFLAGS := -O2 -Wall -ffreestanding -fno-stack-protector -nostdlib -static -mno-avx -fno-asynchronous-unwind-tables \
               -Wl,--build-id=none -Wl,-z,max-page-size=4096
USER_PROGS  := hello hello-pie crash spin

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

$(BUILD)/initrd.npk: $(addprefix $(BUILD)/user/,$(USER_PROGS)) tools/mkinitrd.py
	python3 tools/mkinitrd.py $@ $(foreach p,$(USER_PROGS),bin/$(p)=$(BUILD)/user/$(p)) etc/motd=README.md

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

run: all
	$(QEMU)

# Exit code 33 == kernel self-test passed (isa-debug-exit 0x10 -> (0x10<<1)|1).
test: KARGS := neos.boot.test=1
test: all
	@timeout 30 $(QEMU); rc=$$?; if [ $$rc -eq 33 ]; then echo "TEST OK"; else echo "TEST FAILED (rc=$$rc)"; exit 1; fi
	@$(MAKE) --no-print-directory shelltest
	@$(MAKE) --no-print-directory nettest
	@$(MAKE) --no-print-directory usertest
	@$(MAKE) --no-print-directory agenttest

shelltest: all
	@tools/check-shell.sh

nettest: all
	@tools/check-net.sh

usertest: all
	@tools/check-user.sh

agenttest: all
	@python3 tools/check-agent.py | tail -1

clean:
	rm -rf $(BUILD)

.PHONY: all run test shelltest nettest usertest agenttest clean
