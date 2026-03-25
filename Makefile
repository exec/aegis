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
    -Ikernel/signal \
    -Ikernel/drivers \
    -Ikernel/net

ASFLAGS = -f elf64
LDFLAGS = -T tools/linker.ld -nostdlib

BUILD   = build
ISO_DIR = $(BUILD)/isodir

# ── INIT variable ───────────────────────────────────────────────────────────
# INIT=oksh (default): embeds user/oksh/oksh.elf as init process
# INIT=hello          : embeds user/hello/hello.elf as init process
# INIT=shell          : embeds user/shell/shell.elf as init process
# make shell          : convenience target for INIT=shell run
INIT ?= oksh

ifeq ($(INIT),shell)
INIT_ELF_SRC = user/shell/shell.elf
else ifeq ($(INIT),oksh)
INIT_ELF_SRC = user/oksh/oksh.elf
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
    kernel/arch/x86_64/arch_smap.c \
    kernel/arch/x86_64/acpi.c \
    kernel/arch/x86_64/pcie.c

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

DRIVER_SRCS = \
    kernel/drivers/nvme.c \
    kernel/drivers/xhci.c \
    kernel/drivers/usb_hid.c \
    kernel/drivers/virtio_net.c \
    kernel/drivers/fb.c

NET_SRCS = \
    kernel/net/netdev.c \
    kernel/net/eth.c \
    kernel/net/ip.c \
    kernel/net/udp.c \
    kernel/net/tcp.c

FS_SRCS = \
    kernel/fs/vfs.c \
    kernel/fs/initrd.c \
    kernel/fs/console.c \
    kernel/fs/kbd_vfs.c \
    kernel/fs/pipe.c \
    kernel/fs/blkdev.c \
    kernel/fs/gpt.c \
    kernel/fs/ext2.c \
    kernel/fs/ext2_cache.c \
    kernel/fs/ext2_dir.c

USERSPACE_SRCS = \
    kernel/syscall/syscall.c \
    kernel/syscall/sys_io.c \
    kernel/syscall/sys_memory.c \
    kernel/syscall/sys_process.c \
    kernel/syscall/sys_file.c \
    kernel/syscall/sys_signal.c \
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
    kernel/false_bin.c \
    kernel/wc_bin.c \
    kernel/grep_bin.c \
    kernel/sort_bin.c \
    kernel/mkdir_bin.c \
    kernel/touch_bin.c \
    kernel/rm_bin.c \
    kernel/cp_bin.c \
    kernel/mv_bin.c \
    kernel/whoami_bin.c \
    kernel/oksh_bin.c

# ── Object file lists ─────────────────────────────────────────────────────────
ARCH_OBJS      = $(patsubst kernel/%.c,$(BUILD)/%.o,$(ARCH_SRCS))
CORE_OBJS      = $(patsubst kernel/%.c,$(BUILD)/%.o,$(CORE_SRCS))
MM_OBJS        = $(patsubst kernel/%.c,$(BUILD)/%.o,$(MM_SRCS))
BOOT_OBJ       = $(BUILD)/arch/x86_64/boot.o
ARCH_ASM_OBJS  = $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ARCH_ASMS))
SCHED_OBJS     = $(patsubst kernel/%.c,$(BUILD)/%.o,$(SCHED_SRCS))
FS_OBJS        = $(patsubst kernel/%.c,$(BUILD)/%.o,$(FS_SRCS))
DRIVER_OBJS    = $(patsubst kernel/%.c,$(BUILD)/%.o,$(DRIVER_SRCS))
NET_OBJS       = $(patsubst kernel/%.c,$(BUILD)/%.o,$(NET_SRCS))
USERSPACE_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(USERSPACE_SRCS))
PROG_BIN_OBJS  = $(patsubst kernel/%.c,$(BUILD)/%.o,$(PROG_BIN_SRCS))

ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) $(MM_OBJS) \
           $(SCHED_OBJS) $(FS_OBJS) $(DRIVER_OBJS) $(NET_OBJS) $(USERSPACE_OBJS) $(PROG_BIN_OBJS)

.PHONY: all iso disk run run-fb shell oksh test clean

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
	 NAME=$$(basename $$ELF .elf); \
	 cd $(dir $(INIT_ELF_SRC)) && xxd -i $$ELF | \
	 sed "s/$${NAME}_elf/init_elf/g" > ../../$(INIT_BIN_C) && \
	 echo "const char init_name[] = \"$$NAME\";" >> ../../$(INIT_BIN_C)

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

user/wc/wc.elf:
	$(MAKE) -C user/wc

user/grep/grep.elf:
	$(MAKE) -C user/grep

user/sort/sort.elf:
	$(MAKE) -C user/sort

user/mkdir/mkdir.elf:
	$(MAKE) -C user/mkdir

user/touch/touch.elf:
	$(MAKE) -C user/touch

user/rm/rm.elf:
	$(MAKE) -C user/rm

user/cp/cp.elf:
	$(MAKE) -C user/cp

user/mv/mv.elf:
	$(MAKE) -C user/mv

user/whoami/whoami.elf:
	$(MAKE) -C user/whoami

user/oksh/oksh.elf:
	$(MAKE) -C user/oksh

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

kernel/wc_bin.c: user/wc/wc.elf
	cd user/wc && xxd -i wc.elf > ../../kernel/wc_bin.c

kernel/grep_bin.c: user/grep/grep.elf
	cd user/grep && xxd -i grep.elf > ../../kernel/grep_bin.c

kernel/sort_bin.c: user/sort/sort.elf
	cd user/sort && xxd -i sort.elf > ../../kernel/sort_bin.c

kernel/mkdir_bin.c: user/mkdir/mkdir.elf
	cd user/mkdir && xxd -i mkdir.elf > ../../kernel/mkdir_bin.c

kernel/touch_bin.c: user/touch/touch.elf
	cd user/touch && xxd -i touch.elf > ../../kernel/touch_bin.c

kernel/rm_bin.c: user/rm/rm.elf
	cd user/rm && xxd -i rm.elf > ../../kernel/rm_bin.c

kernel/cp_bin.c: user/cp/cp.elf
	cd user/cp && xxd -i cp.elf > ../../kernel/cp_bin.c

kernel/mv_bin.c: user/mv/mv.elf
	cd user/mv && xxd -i mv.elf > ../../kernel/mv_bin.c

kernel/whoami_bin.c: user/whoami/whoami.elf
	cd user/whoami && xxd -i whoami.elf > ../../kernel/whoami_bin.c

kernel/oksh_bin.c: user/oksh/oksh.elf
	cd user/oksh && xxd -i oksh.elf > ../../kernel/oksh_bin.c

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

DISK = $(BUILD)/disk.img
SGDISK     = /usr/sbin/sgdisk
# nvme0p1: sgdisk aligns start to LBA 2048, end 122879 = 120832 sectors = ~59 MB
P1_SECTORS = 120832

# All user ELFs required before disk can be populated.
# debugfs silently skips `write` commands whose source file is missing,
# producing a disk with an empty /bin and no build error — so we list
# them all as prerequisites here.
DISK_USER_BINS = \
	user/shell/shell.elf user/ls/ls.elf user/cat/cat.elf \
	user/echo/echo.elf user/pwd/pwd.elf user/uname/uname.elf \
	user/clear/clear.elf user/true/true.elf user/false/false.elf \
	user/wc/wc.elf user/grep/grep.elf user/sort/sort.elf \
	user/mv/mv.elf user/cp/cp.elf user/rm/rm.elf \
	user/mkdir/mkdir.elf user/touch/touch.elf

disk: $(DISK)

$(DISK): $(DISK_USER_BINS)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=64 2>/dev/null
	$(SGDISK) \
	    --new=1:34:122879   --typecode=1:8300 --change-name=1:aegis-root \
	    --new=2:122880:0    --typecode=2:8200 --change-name=2:aegis-swap \
	    $(DISK)
	dd if=/dev/zero of=/tmp/aegis-p1.img bs=512 count=$(P1_SECTORS) 2>/dev/null
	/sbin/mke2fs -t ext2 -F -L aegis-root /tmp/aegis-p1.img
	printf 'mkdir /bin\nmkdir /etc\nmkdir /tmp\nmkdir /home\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	@printf "Welcome to Aegis\n" > /tmp/aegis-motd
	printf 'write user/shell/shell.elf /bin/sh\nwrite user/ls/ls.elf /bin/ls\nwrite user/cat/cat.elf /bin/cat\nwrite user/echo/echo.elf /bin/echo\nwrite user/pwd/pwd.elf /bin/pwd\nwrite user/uname/uname.elf /bin/uname\nwrite user/clear/clear.elf /bin/clear\nwrite user/true/true.elf /bin/true\nwrite user/false/false.elf /bin/false\nwrite user/wc/wc.elf /bin/wc\nwrite user/grep/grep.elf /bin/grep\nwrite user/sort/sort.elf /bin/sort\nwrite user/mv/mv.elf /bin/mv\nwrite user/cp/cp.elf /bin/cp\nwrite user/rm/rm.elf /bin/rm\nwrite user/mkdir/mkdir.elf /bin/mkdir\nwrite user/touch/touch.elf /bin/touch\nwrite /tmp/aegis-motd /etc/motd\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	# Auth files for login
	printf 'root:x:0:0:root:/root:/bin/oksh\n' > /tmp/aegis-passwd
	printf 'root:$$6$$5a3b9c1d2e4f6789$$fvwyIjdmyvB59hifGMRFrcwhBb4cH0.3nRy2j2LpCk.aNIFNyvYQJ36Bsl94miFbD/JHICz8O1dXoegZ0OmOg.:19000:0:99999:7:::\n' > /tmp/aegis-shadow
	printf 'root:x:0:root\nwheel:x:999:root\n' > /tmp/aegis-group
	printf 'write /tmp/aegis-passwd /etc/passwd\nwrite /tmp/aegis-shadow /etc/shadow\nwrite /tmp/aegis-group /etc/group\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	rm -f /tmp/aegis-passwd /tmp/aegis-shadow /tmp/aegis-group
	dd if=/tmp/aegis-p1.img of=$(DISK) bs=512 seek=2048 conv=notrunc 2>/dev/null
	@rm -f /tmp/aegis-p1.img /tmp/aegis-motd
	@echo "Disk image created: $(DISK)"

comma := ,
NVME_FLAGS = $(if $(wildcard $(DISK)),\
    -drive file=$(DISK)$(comma)if=none$(comma)id=nvme0 \
    -device nvme$(comma)drive=nvme0$(comma)serial=aegis0,)

run: iso
	qemu-system-x86_64 \
	    -machine q35 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga std -no-reboot -m 128M \
	    $(NVME_FLAGS) \
	    -device qemu-xhci -device usb-kbd \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

# run-fb: boot with virtio-vga display (framebuffer) instead of legacy VGA.
# Uses USB keyboard (-device usb-kbd) because virtio-vga replaces the legacy
# VGA device that normally coexists with the i8042 PS/2 controller; the USB
# HID path (xHCI → usb-kbd → kbd_usb_inject) is always reliable here.
# Do NOT combine with -vga std: two VGA adapters confuse GRUB's video init.
run-fb: iso
	qemu-system-x86_64 \
	    -machine q35 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga none -device virtio-vga -no-reboot -m 128M \
	    $(NVME_FLAGS) \
	    -device qemu-xhci -device usb-kbd \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

shell:
	$(MAKE) INIT=shell run

oksh:
	$(MAKE) INIT=oksh run

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
	rm -f kernel/wc_bin.c kernel/grep_bin.c kernel/sort_bin.c
	rm -f kernel/mkdir_bin.c kernel/touch_bin.c kernel/rm_bin.c
	rm -f kernel/cp_bin.c kernel/mv_bin.c
	rm -f kernel/whoami_bin.c kernel/oksh_bin.c
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
	$(MAKE) -C user/wc clean
	$(MAKE) -C user/grep clean
	$(MAKE) -C user/sort clean
	$(MAKE) -C user/mkdir clean
	$(MAKE) -C user/touch clean
	$(MAKE) -C user/rm clean
	$(MAKE) -C user/cp clean
	$(MAKE) -C user/mv clean
	$(MAKE) -C user/whoami clean 2>/dev/null; true
	$(MAKE) -C user/oksh clean 2>/dev/null; true
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
