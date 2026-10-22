CC      := gcc
LD      := ld
NASM    := nasm
BUILD   := build
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-pic -fno-pie \
           -fno-stack-protector -mno-red-zone -mgeneral-regs-only -Ikernel
LDFLAGS := -T kernel/linker.ld -nostdlib -static -z max-page-size=0x1000

CSRC := $(wildcard kernel/*.c)
OBJS := $(BUILD)/boot.o $(BUILD)/isr.o $(patsubst kernel/%.c,$(BUILD)/%.o,$(CSRC))

AGENT_CHAN ?= -serial unix:$(BUILD)/agent.sock,server,nowait
NIC ?= -nic user,model=e1000
KARGS ?=
QEMU = qemu-system-x86_64 -kernel $(BUILD)/matrix32.elf -m 128 -display none -append "$(KARGS)" \
        -cpu max -serial stdio $(AGENT_CHAN) -no-reboot $(NIC) -device isa-debug-exit,iobase=0xf4,iosize=0x04

all: $(BUILD)/matrix32.elf

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
	@$(MAKE) --no-print-directory agenttest

shelltest: all
	@tools/check-shell.sh

nettest: all
	@tools/check-net.sh

agenttest: all
	@python3 tools/check-agent.py | tail -1

clean:
	rm -rf $(BUILD)

.PHONY: all run test shelltest nettest agenttest clean
