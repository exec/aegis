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
    -fno-omit-frame-pointer \
    -g \
    -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 \
    -Ikernel/core \
    -Ikernel/cap \
    -Ikernel/mm \
    -Ikernel/sched \
    -Ikernel/proc \
    -Ikernel/syscall \
    -Ikernel/elf \
    -Ikernel/fs \
    -Ikernel/signal

ASFLAGS = -f elf64
LDFLAGS = -T tools/linker.ld -nostdlib

BUILD   = build
ISO_DIR = $(BUILD)/isodir

# ── INIT variable ───────────────────────────────────────────────────────────
# INIT=hello (default): embeds user/hello/hello.elf as init process
# INIT=shell           : embeds user/shell/shell.elf as init process
# make shell           : convenience target for INIT=shell run
INIT ?= hello

ifeq ($(INIT),shell)
INIT_ELF_SRC = user/shell/shell.elf
else
INIT_ELF_SRC = user/hello/hello.elf
endif

INIT_BIN_C = kernel/init_bin.c
INIT_STAMP  = .init_stamp_$(INIT)

# ── Source lists ─────────────────────────────────────────────────────────────
ARCH_SRCS = \
    kernel/arch/x86_64/arch.c \
    kernel/arch/x86_64/arch_exit.c \
    kernel/arch/x86_64/arch_mm.c \
    kernel/arch/x86_64/arch_vmm.c \
    kernel/arch/x86_64/serial.c \
    kernel/arch/x86_64/vga.c \
    kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/pic.c \
    kernel/arch/x86_64/pit.c \
    kernel/arch/x86_64/kbd.c \
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/tss.c \
    kernel/arch/x86_64/arch_syscall.c \
    kernel/arch/x86_64/arch_smap.c

CORE_SRCS = \
    kernel/core/main.c \
    kernel/core/printk.c \
    kernel/signal/signal.c

MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kva.c

BOOT_SRC = kernel/arch/x86_64/boot.asm
CAP_LIB  = kernel/cap/target/x86_64-unknown-none/release/libcap.a

ARCH_ASMS = \
    kernel/arch/x86_64/isr.asm \
    kernel/arch/x86_64/ctx_switch.asm \
    kernel/arch/x86_64/syscall_entry.asm

SCHED_SRCS = kernel/sched/sched.c

FS_SRCS = \
    kernel/fs/vfs.c \
    kernel/fs/initrd.c \
    kernel/fs/console.c \
    kernel/fs/kbd_vfs.c \
    kernel/fs/pipe.c

USERSPACE_SRCS = \
    kernel/syscall/syscall.c \
    kernel/proc/proc.c \
    kernel/elf/elf.c \
    $(INIT_BIN_C)

# Programs embedded in initrd as binary C arrays
PROG_BIN_SRCS = \
    kernel/shell_bin.c \
    kernel/ls_bin.c \
    kernel/cat_bin.c \
    kernel/echo_bin.c \
    kernel/pwd_bin.c \
    kernel/uname_bin.c \
    kernel/clear_bin.c \
    kernel/true_bin.c \
    kernel/false_bin.c

# ── Object file lists ─────────────────────────────────────────────────────────
ARCH_OBJS      = $(patsubst kernel/%.c,$(BUILD)/%.o,$(ARCH_SRCS))
CORE_OBJS      = $(patsubst kernel/%.c,$(BUILD)/%.o,$(CORE_SRCS))
MM_OBJS        = $(patsubst kernel/%.c,$(BUILD)/%.o,$(MM_SRCS))
BOOT_OBJ       = $(BUILD)/arch/x86_64/boot.o
ARCH_ASM_OBJS  = $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ARCH_ASMS))
SCHED_OBJS     = $(patsubst kernel/%.c,$(BUILD)/%.o,$(SCHED_SRCS))
FS_OBJS        = $(patsubst kernel/%.c,$(BUILD)/%.o,$(FS_SRCS))
USERSPACE_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(USERSPACE_SRCS))
PROG_BIN_OBJS  = $(patsubst kernel/%.c,$(BUILD)/%.o,$(PROG_BIN_SRCS))

ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) $(MM_OBJS) \
           $(SCHED_OBJS) $(FS_OBJS) $(USERSPACE_OBJS) $(PROG_BIN_OBJS)

.PHONY: all iso run shell test clean

all: $(BUILD)/aegis.elf

# ── Generic C compilation rule ────────────────────────────────────────────────
$(BUILD)/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Boot and arch ASM ─────────────────────────────────────────────────────────
$(BOOT_OBJ): $(BOOT_SRC)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/arch/x86_64/%.o: kernel/arch/x86_64/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# ── Capability library (Rust) ─────────────────────────────────────────────────
$(CAP_LIB): kernel/cap/src/lib.rs kernel/cap/Cargo.toml
	$(CARGO) build --release \
	    --target x86_64-unknown-none \
	    --manifest-path kernel/cap/Cargo.toml

# ── INIT binary (hello or shell) ──────────────────────────────────────────────
# Stamp file detects INIT variable changes and forces rebuild of init_bin.c
$(INIT_STAMP):
	@rm -f .init_stamp_*
	@touch $@

.PHONY: $(INIT_STAMP)

user/hello/hello.elf:
	$(MAKE) -C user/hello

user/shell/shell.elf:
	$(MAKE) -C user/shell

$(INIT_BIN_C): $(INIT_STAMP) $(INIT_ELF_SRC)
	@ELF=$(notdir $(INIT_ELF_SRC)); \
	 cd $(dir $(INIT_ELF_SRC)) && xxd -i $$ELF | \
	 sed "s/$$(basename $$ELF .elf)_elf/init_elf/g" > ../../$(INIT_BIN_C)

# ── Program ELF binaries ──────────────────────────────────────────────────────
user/ls/ls.elf:
	$(MAKE) -C user/ls

user/cat/cat.elf:
	$(MAKE) -C user/cat

user/echo/echo.elf:
	$(MAKE) -C user/echo

user/pwd/pwd.elf:
	$(MAKE) -C user/pwd

user/uname/uname.elf:
	$(MAKE) -C user/uname

user/clear/clear.elf:
	$(MAKE) -C user/clear

user/true/true.elf:
	$(MAKE) -C user/true

user/false/false.elf:
	$(MAKE) -C user/false

# ── Program binary C arrays ───────────────────────────────────────────────────
kernel/shell_bin.c: user/shell/shell.elf
	cd user/shell && xxd -i shell.elf > ../../kernel/shell_bin.c

kernel/ls_bin.c: user/ls/ls.elf
	cd user/ls && xxd -i ls.elf > ../../kernel/ls_bin.c

kernel/cat_bin.c: user/cat/cat.elf
	cd user/cat && xxd -i cat.elf > ../../kernel/cat_bin.c

kernel/echo_bin.c: user/echo/echo.elf
	cd user/echo && xxd -i echo.elf > ../../kernel/echo_bin.c

kernel/pwd_bin.c: user/pwd/pwd.elf
	cd user/pwd && xxd -i pwd.elf > ../../kernel/pwd_bin.c

kernel/uname_bin.c: user/uname/uname.elf
	cd user/uname && xxd -i uname.elf > ../../kernel/uname_bin.c

kernel/clear_bin.c: user/clear/clear.elf
	cd user/clear && xxd -i clear.elf > ../../kernel/clear_bin.c

# true and false: rename symbols to avoid C keyword conflicts
# true.elf -> true_bin_elf / true_bin_elf_len
# false.elf -> false_bin_elf / false_bin_elf_len
kernel/true_bin.c: user/true/true.elf
	cd user/true && xxd -i true.elf | \
	  sed 's/unsigned char true_elf/unsigned char true_bin_elf/g; s/unsigned int true_elf_len/unsigned int true_bin_elf_len/g' \
	  > ../../kernel/true_bin.c

kernel/false_bin.c: user/false/false.elf
	cd user/false && xxd -i false.elf | \
	  sed 's/unsigned char false_elf/unsigned char false_bin_elf/g; s/unsigned int false_elf_len/unsigned int false_bin_elf_len/g' \
	  > ../../kernel/false_bin.c

# ── Final link ────────────────────────────────────────────────────────────────
$(BUILD)/aegis.elf: $(INIT_BIN_C) $(PROG_BIN_SRCS) $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)

$(BUILD)/aegis.iso: $(BUILD)/aegis.elf tools/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/aegis.elf $(ISO_DIR)/boot/aegis.elf
	cp tools/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

# ── Run targets ───────────────────────────────────────────────────────────────
iso: $(BUILD)/aegis.iso

run: iso
	qemu-system-x86_64 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga std -no-reboot -m 128M \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

shell:
	$(MAKE) INIT=shell run

# ── Debug targets ─────────────────────────────────────────────────────────────
# make gdb      — boot kernel frozen at first instruction; attach GDB.
#                 Serial output captured to build/debug.log.
#                 In GDB: type 'c' to start, 'Ctrl-C' to pause, 'bt' for stack.
# make sym ADDR=0x... — resolve a raw kernel address to file:line
gdb: iso
	@echo "[GDB] QEMU GDB server on :1234 — serial -> $(BUILD)/debug.log"
	@qemu-system-x86_64 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial file:$(BUILD)/debug.log \
	    -vga std -no-reboot -m 128M \
	    -s -S \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -display none & \
	sleep 0.3 && gdb -x tools/aegis.gdb

sym:
	@addr2line -e $(BUILD)/aegis.elf -f -p $(ADDR)

test: iso
	@bash tests/run_tests.sh

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
	rm -f kernel/init_bin.c kernel/hello_bin.c
	rm -f kernel/shell_bin.c kernel/ls_bin.c kernel/cat_bin.c kernel/echo_bin.c
	rm -f kernel/pwd_bin.c kernel/uname_bin.c kernel/clear_bin.c
	rm -f kernel/true_bin.c kernel/false_bin.c
	rm -f .init_stamp_*
	$(MAKE) -C user/init clean 2>/dev/null; true
	$(MAKE) -C user/hello clean
	$(MAKE) -C user/shell clean
	$(MAKE) -C user/ls clean
	$(MAKE) -C user/cat clean
	$(MAKE) -C user/echo clean
	$(MAKE) -C user/pwd clean
	$(MAKE) -C user/uname clean
	$(MAKE) -C user/clear clean
	$(MAKE) -C user/true clean
	$(MAKE) -C user/false clean
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
