CC    = x86_64-elf-gcc
AS    = nasm
LD    = x86_64-elf-ld
CARGO = cargo +nightly

# -nostdinc: exclude ALL system includes (kernel provides its own or uses none)
# -isystem ...: add back only GCC's own freestanding headers (stdint.h, stddef.h)
# -mno-red-zone: required for x86_64 kernels (interrupt handlers use stack below RSP)
# -mno-mmx/sse/sse2: no floating point or SIMD in kernel
# -fno-stack-protector: no canary (no runtime support for it)
# -Wall -Wextra -Werror: warnings are errors
GCC_INCLUDE := $(shell $(CC) -print-file-name=include)
CFLAGS = \
    -ffreestanding -nostdlib -nostdinc \
    -isystem $(GCC_INCLUDE) \
    -mcmodel=kernel \
    -fno-pie -fno-pic \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector \
    -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 \
    -Ikernel/core \
    -Ikernel/cap \
    -Ikernel/mm

ASFLAGS = -f elf64
LDFLAGS = -T tools/linker.ld -nostdlib

BUILD   = build
ISO_DIR = $(BUILD)/isodir

ARCH_SRCS = \
    kernel/arch/x86_64/arch.c \
    kernel/arch/x86_64/arch_exit.c \
    kernel/arch/x86_64/arch_mm.c \
    kernel/arch/x86_64/arch_vmm.c \
    kernel/arch/x86_64/serial.c \
    kernel/arch/x86_64/vga.c

CORE_SRCS = \
    kernel/core/main.c \
    kernel/core/printk.c

MM_SRCS = \
    kernel/mm/pmm.c

BOOT_SRC = kernel/arch/x86_64/boot.asm
CAP_LIB  = kernel/cap/target/x86_64-unknown-none/release/libcap.a

ARCH_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(ARCH_SRCS))
CORE_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(CORE_SRCS))
MM_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(MM_SRCS))
BOOT_OBJ  = $(BUILD)/arch/x86_64/boot.o

ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(CORE_OBJS) $(MM_OBJS)

.PHONY: all iso run test clean

all: $(BUILD)/aegis.elf

$(BUILD)/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_OBJ): $(BOOT_SRC)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(CAP_LIB): kernel/cap/src/lib.rs kernel/cap/Cargo.toml
	$(CARGO) build --release \
	    --target x86_64-unknown-none \
	    --manifest-path kernel/cap/Cargo.toml

$(BUILD)/aegis.elf: $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)

$(BUILD)/aegis.iso: $(BUILD)/aegis.elf tools/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/aegis.elf $(ISO_DIR)/boot/aegis.elf
	cp tools/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

iso: $(BUILD)/aegis.iso

run: iso
	qemu-system-x86_64 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga std -no-reboot -m 128M \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

test: iso
	@bash tests/run_tests.sh

clean:
	rm -rf $(BUILD)
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
