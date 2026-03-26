# Makefile — ARM64 kernel build
#
# Usage:
#   make          — build build/aegis-arm64.elf
#   make run      — build + boot on QEMU virt with serial on stdio
#   make test     — build + boot headless, check for [PMM] OK
#   make clean    — remove build artifacts

CC      = aarch64-elf-gcc
LD      = aarch64-elf-ld

# Root of the Aegis repo (two levels up from kernel/arch/arm64/)
REPO    = ../../..

CFLAGS  = -ffreestanding -nostdlib -nostdinc \
          -isystem $(shell $(CC) -print-file-name=include) \
          -Wall -Wextra -Werror -O2 -g \
          -mgeneral-regs-only \
          -I. \
          -I$(REPO)/kernel/core \
          -I$(REPO)/kernel/mm \
          -I$(REPO)/kernel/drivers

ASFLAGS = -g

BUILD   = build
ELF     = $(BUILD)/aegis-arm64.elf

# ARM64 arch-specific sources
ARCH_OBJS = $(BUILD)/boot.o \
            $(BUILD)/uart_pl011.o \
            $(BUILD)/main.o \
            $(BUILD)/arch_mm.o \
            $(BUILD)/stubs.o

# Shared kernel sources (arch-agnostic)
SHARED_OBJS = $(BUILD)/printk.o \
              $(BUILD)/pmm.o

OBJS = $(ARCH_OBJS) $(SHARED_OBJS)

.PHONY: all run test clean

all: $(ELF)

# ARM64 arch sources
$(BUILD)/boot.o: boot.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/uart_pl011.o: uart_pl011.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch_mm.o: arch_mm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/stubs.o: stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Shared kernel sources
$(BUILD)/printk.o: $(REPO)/kernel/core/printk.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: $(REPO)/kernel/mm/pmm.c | $(BUILD)
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
	@gtimeout 10s qemu-system-aarch64 \
	  -machine virt -cpu cortex-a72 \
	  -m 128M -nographic \
	  -kernel $(ELF) 2>/dev/null \
	  | tee /tmp/aegis-arm64-serial.txt || true
	@grep -q '\[PMM\] OK' /tmp/aegis-arm64-serial.txt && \
	  echo "PASS: [PMM] OK found" || \
	  (echo "FAIL: [PMM] OK not found"; cat /tmp/aegis-arm64-serial.txt; exit 1)

clean:
	rm -rf $(BUILD)
