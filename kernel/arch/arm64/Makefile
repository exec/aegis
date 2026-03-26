# Makefile — ARM64 boot stub build
#
# Usage:
#   make          — build kernel/arch/arm64/build/aegis-arm64.elf
#   make run      — build + boot on QEMU virt with serial on stdio
#   make test     — build + boot headless, check for [SERIAL] OK
#   make clean    — remove build artifacts

CC      = aarch64-elf-gcc
LD      = aarch64-elf-ld
OBJCOPY = aarch64-elf-objcopy

CFLAGS  = -ffreestanding -nostdlib -nostdinc \
          -isystem $(shell $(CC) -print-file-name=include) \
          -Wall -Wextra -Werror -O2 -g \
          -mgeneral-regs-only

ASFLAGS = -g

BUILD   = build

OBJS    = $(BUILD)/boot.o $(BUILD)/uart_pl011.o $(BUILD)/main.o
ELF     = $(BUILD)/aegis-arm64.elf

.PHONY: all run test clean

all: $(ELF)

$(BUILD)/boot.o: boot.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/uart_pl011.o: uart_pl011.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) linker.ld
	$(LD) -T linker.ld -nostdlib -o $@ $(OBJS)

$(BUILD):
	mkdir -p $(BUILD)

run: $(ELF)
	qemu-system-aarch64 \
	  -machine virt -cpu cortex-a72 \
	  -m 128M -nographic \
	  -kernel $(ELF)

test: $(ELF)
	@echo "--- arm64 boot test ---"
	@gtimeout 5s qemu-system-aarch64 \
	  -machine virt -cpu cortex-a72 \
	  -m 128M -nographic \
	  -kernel $(ELF) 2>/dev/null \
	  | tee /tmp/aegis-arm64-serial.txt || true
	@grep -q '\[SERIAL\] OK' /tmp/aegis-arm64-serial.txt && \
	  echo "PASS: [SERIAL] OK found" || \
	  (echo "FAIL: [SERIAL] OK not found"; exit 1)

clean:
	rm -rf $(BUILD)
