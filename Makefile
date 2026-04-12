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
    -Ikernel/fs \
    -Ikernel/tty \
    -Ikernel/signal \
    -Ikernel/drivers \
    -Ikernel/net

ASFLAGS = -f elf64
LDFLAGS = -T tools/linker.ld -nostdlib
OBJCOPY = x86_64-elf-objcopy

BUILD   = build
ISO_DIR = $(BUILD)/isodir
DISK    = $(BUILD)/disk.img
ROOTFS  = $(BUILD)/rootfs.img
SGDISK  = /usr/sbin/sgdisk

# Init process is always vigil.

# ── Kernel source lists ─────────────────────────────────��───────────────────
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
    kernel/arch/x86_64/ps2_mouse.c \
    kernel/arch/x86_64/lapic.c \
    kernel/arch/x86_64/ioapic.c \
    kernel/arch/x86_64/smp.c \
    kernel/arch/x86_64/tlb.c \
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/tss.c \
    kernel/arch/x86_64/arch_syscall.c \
    kernel/arch/x86_64/arch_smap.c \
    kernel/arch/x86_64/acpi.c \
    kernel/arch/x86_64/pcie.c

CORE_SRCS = \
    kernel/core/main.c \
    kernel/core/printk.c \
    kernel/core/random.c \
    kernel/cap/cap_policy.c

MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kva.c \
    kernel/mm/vma.c

SCHED_SRCS  = kernel/sched/sched.c kernel/sched/waitq.c
SIGNAL_SRCS = kernel/signal/signal.c
TTY_SRCS    = kernel/tty/tty.c kernel/tty/pty.c

FS_SRCS = \
    kernel/fs/fd_table.c kernel/fs/vfs.c kernel/fs/initrd.c \
    kernel/fs/console.c kernel/fs/kbd_vfs.c kernel/fs/pipe.c \
    kernel/fs/blkdev.c kernel/fs/gpt.c \
    kernel/fs/ext2.c kernel/fs/ext2_cache.c kernel/fs/ext2_dir.c kernel/fs/ext2_vfs.c \
    kernel/fs/ramfs.c kernel/fs/procfs.c kernel/fs/memfd.c \
    kernel/fs/poll_test.c

DRIVER_SRCS = \
    kernel/drivers/nvme.c kernel/drivers/xhci.c \
    kernel/drivers/usb_hid.c kernel/drivers/usb_mouse.c \
    kernel/drivers/virtio_net.c kernel/drivers/rtl8169.c \
    kernel/drivers/fb.c kernel/drivers/ramdisk.c

NET_SRCS = \
    kernel/net/netdev.c kernel/net/eth.c kernel/net/ip.c \
    kernel/net/udp.c kernel/net/tcp.c kernel/net/socket.c \
    kernel/net/unix_socket.c kernel/net/epoll.c

USERSPACE_SRCS = \
    kernel/syscall/syscall.c kernel/syscall/sys_io.c \
    kernel/syscall/sys_memory.c kernel/syscall/sys_process.c \
    kernel/syscall/sys_exec.c kernel/syscall/sys_identity.c \
    kernel/syscall/sys_cap.c kernel/syscall/sys_time.c \
    kernel/syscall/sys_file.c kernel/syscall/sys_dir.c \
    kernel/syscall/sys_meta.c kernel/syscall/sys_signal.c \
    kernel/syscall/sys_socket.c kernel/syscall/sys_random.c \
    kernel/syscall/sys_disk.c kernel/syscall/futex.c \
    kernel/proc/proc.c kernel/proc/elf.c

BOOT_SRC  = kernel/arch/x86_64/boot.asm
CAP_LIB   = kernel/cap/target/x86_64-unknown-none/release/libcap.a

ARCH_ASMS = \
    kernel/arch/x86_64/isr.asm \
    kernel/arch/x86_64/ctx_switch.asm \
    kernel/arch/x86_64/syscall_entry.asm \
    kernel/arch/x86_64/ap_trampoline.asm

# ── Object file lists ───────────���────────────────────────────────��───────────
ARCH_OBJS      = $(patsubst kernel/%.c,$(BUILD)/%.o,$(ARCH_SRCS))
CORE_OBJS      = $(patsubst kernel/%.c,$(BUILD)/%.o,$(CORE_SRCS))
MM_OBJS        = $(patsubst kernel/%.c,$(BUILD)/%.o,$(MM_SRCS))
BOOT_OBJ       = $(BUILD)/arch/x86_64/boot.o
ARCH_ASM_OBJS  = $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ARCH_ASMS))
SCHED_OBJS     = $(patsubst kernel/%.c,$(BUILD)/%.o,$(SCHED_SRCS))
SIGNAL_OBJS    = $(patsubst kernel/%.c,$(BUILD)/%.o,$(SIGNAL_SRCS))
TTY_OBJS       = $(patsubst kernel/%.c,$(BUILD)/%.o,$(TTY_SRCS))
FS_OBJS        = $(patsubst kernel/%.c,$(BUILD)/%.o,$(FS_SRCS))
DRIVER_OBJS    = $(patsubst kernel/%.c,$(BUILD)/%.o,$(DRIVER_SRCS))
NET_OBJS       = $(patsubst kernel/%.c,$(BUILD)/%.o,$(NET_SRCS))
USERSPACE_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(USERSPACE_SRCS))

# ── Initrd blobs (statically linked, embedded in kernel via objcopy) ─────────
BLOB_NAMES = login vigil shell echo cat ls init
BLOB_OBJS  = $(patsubst %,$(BUILD)/blobs/%.o,$(BLOB_NAMES))

ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) $(SIGNAL_OBJS) \
           $(MM_OBJS) $(SCHED_OBJS) $(TTY_OBJS) $(FS_OBJS) $(DRIVER_OBJS) \
           $(NET_OBJS) $(USERSPACE_OBJS) $(BLOB_OBJS)

.PHONY: all iso disk rootfs run run-fb shell login test test-q35 install-test clean gdb sym curl_bin build-musl

all: $(BUILD)/aegis.elf

# ── Generic kernel compilation rules ──────���──────────────────────────────────
$(BUILD)/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_OBJ): $(BOOT_SRC)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/arch/x86_64/%.o: kernel/arch/x86_64/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# ── Capability library (Rust) ──────────���─────────────────────────────────────
$(CAP_LIB): kernel/cap/src/lib.rs kernel/cap/Cargo.toml
	$(CARGO) build --release \
	    --target x86_64-unknown-none \
	    --manifest-path kernel/cap/Cargo.toml

# ── User program builds ─��──────────────────────────────────���────────────────
# musl shared library (dynamic linker)
MUSL_BUILT = build/musl-dynamic/usr/lib/libc.so

build/musl-dynamic/usr/lib/libc.so:
	bash tools/build-musl.sh

build-musl: $(MUSL_BUILT)

# Simple user programs: depend only on musl, built by their own Makefile.
# Add new programs here — that's it. No other list to update.
SIMPLE_USER_PROGS = \
    ls cat echo pwd uname clear true false wc grep sort \
    mkdir touch rm cp mv whoami ln chmod chown readlink \
    shutdown reboot login stsh httpd nettest polltest

# Generate rules: user/bin/foo/foo.elf depends on musl, built via sub-make
define SIMPLE_USER_RULE
user/bin/$(1)/$(1).elf: $$(MUSL_BUILT)
	$$(MAKE) -C user/bin/$(1)
endef
$(foreach p,$(SIMPLE_USER_PROGS),$(eval $(call SIMPLE_USER_RULE,$(p))))

# Programs with non-.elf output names
user/bin/vigil/vigil: user/bin/vigil/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/vigil

user/bin/vigictl/vigictl: user/bin/vigictl/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/vigictl

user/bin/dhcp/dhcp: user/bin/dhcp/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/dhcp

user/bin/chronos/chronos: user/bin/chronos/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/chronos

# Static binary (no musl)
user/bin/shell/shell.elf:
	$(MAKE) -C user/bin/shell

# Libraries
user/lib/glyph/libglyph.a: $(wildcard user/lib/glyph/*.c user/lib/glyph/*.h) $(MUSL_BUILT)
	$(MAKE) -C user/lib/glyph

user/lib/libauth/libauth.a: user/lib/libauth/auth.c user/lib/libauth/auth.h
	$(MAKE) -C user/lib/libauth

user/lib/citadel/libcitadel.a: $(wildcard user/lib/citadel/*.c user/lib/citadel/*.h) user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/lib/citadel

user/lib/libinstall/libinstall.a: $(wildcard user/lib/libinstall/*.c user/lib/libinstall/*.h) $(MUSL_BUILT)
	$(MAKE) -C user/lib/libinstall

# Programs with extra library dependencies
user/bin/lumen/lumen.elf: $(wildcard user/bin/lumen/*.c user/bin/lumen/*.h) user/lib/glyph/libglyph.a user/lib/citadel/libcitadel.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/lumen

user/bin/bastion/bastion.elf: user/bin/bastion/main.c user/lib/glyph/libglyph.a user/lib/libauth/libauth.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/bastion

user/bin/installer/installer.elf: user/bin/installer/main.c user/lib/libinstall/libinstall.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/installer

user/bin/gui-installer/gui-installer.elf: user/bin/gui-installer/main.c user/lib/glyph/libglyph.a user/lib/libinstall/libinstall.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/gui-installer

# BearSSL + curl (external builds)
build/bearssl-install/lib/libbearssl.a:
	bash tools/build-bearssl.sh

build/curl/curl: build/bearssl-install/lib/libbearssl.a
	bash tools/build-curl.sh || (mkdir -p build/curl && echo '#!/bin/sh' > $@ && chmod +x $@ && echo "[curl] build failed — using stub")

curl_bin: build/curl/curl

# Rune text editor (external Rust build)
user/bin/rune:
	bash tools/build-rune.sh

# ── Binary blob embedding (objcopy) ��────────────────────��───────────────────
# Pattern rule: build/blobs/%.bin → build/blobs/%.o
$(BUILD)/blobs/%.o: $(BUILD)/blobs/%.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $*.bin $*.o

# Blob source mapping: name → ELF path.
# Add new blobs here — one line each. The macro generates the copy rule.
BLOB_MAP_login = user/bin/login/login.elf
BLOB_MAP_vigil = user/bin/vigil/vigil
BLOB_MAP_shell = user/bin/shell/shell.elf
BLOB_MAP_init  = user/bin/vigil/vigil

define BLOB_COPY_RULE
$(BUILD)/blobs/$(1).bin: $$(BLOB_MAP_$(1))
	@mkdir -p $(BUILD)/blobs
	@cp $$< $$@
endef
$(foreach b,login vigil shell init,$(eval $(call BLOB_COPY_RULE,$(b))))

# Static initrd copies of echo/cat/ls (built from source, no dynamic linker)
define BLOB_STATIC_RULE
$(BUILD)/blobs/$(1).bin: user/bin/$(1)/main.c
	@mkdir -p $(BUILD)/blobs
	musl-gcc -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none -o $$@ $$<
endef
$(foreach b,echo cat ls,$(eval $(call BLOB_STATIC_RULE,$(b))))

# ── Final link ──────���─────────────────────────────────────��──────────────────
$(BUILD)/aegis.elf: $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)

# ── EFI GRUB + ESP image for installer ─────���────────────────────────────────
GRUB_EFI = $(BUILD)/BOOTX64.EFI
ESP_IMG  = $(BUILD)/esp.img

$(GRUB_EFI):
	@mkdir -p $(BUILD)
	grub-mkimage -O x86_64-efi -o $(GRUB_EFI) \
	    -p /EFI/BOOT \
	    part_gpt ext2 fat normal multiboot2 boot configfile \
	    search search_fs_file \
	    all_video efi_gop efi_uga video video_bochs video_cirrus gfxterm \
	    font gfxmenu jpeg png

$(ESP_IMG): $(GRUB_EFI) $(GRUB_FONT) tools/grub-installed.cfg
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(ESP_IMG) bs=512 count=65536 2>/dev/null
	/sbin/mkfs.fat -F 16 $(ESP_IMG) >/dev/null 2>&1
	mmd -i $(ESP_IMG) ::EFI
	mmd -i $(ESP_IMG) ::EFI/BOOT
	mcopy -i $(ESP_IMG) $(GRUB_EFI) ::EFI/BOOT/BOOTX64.EFI
	mcopy -i $(ESP_IMG) /usr/share/grub/unicode.pf2 ::EFI/BOOT/unicode.pf2
	mcopy -i $(ESP_IMG) tools/grub-bg.jpg ::EFI/BOOT/bg.jpg
	@if [ -f assets/wallpaper.png ]; then mcopy -i $(ESP_IMG) assets/wallpaper.png ::EFI/BOOT/wallpaper.png; fi
	@if [ -s $(GRUB_FONT) ]; then mcopy -i $(ESP_IMG) $(GRUB_FONT) ::EFI/BOOT/font.pf2; fi
	mcopy -i $(ESP_IMG) tools/grub-installed.cfg ::EFI/BOOT/grub.cfg

# ── Wallpaper / logo conversion ──���───────────────────────────────────────────
$(BUILD)/logo.raw: tools/aegis-logo.png
	@mkdir -p $(BUILD)
	@if command -v python3 >/dev/null 2>&1 && [ -f tools/convert-logo.py ]; then \
	    python3 tools/convert-logo.py $< $@; \
	else touch $@; fi

$(BUILD)/claude.raw: assets/claude-white.png
	@mkdir -p $(BUILD)
	@if command -v python3 >/dev/null 2>&1 && [ -f tools/convert-logo.py ]; then \
	    python3 tools/convert-logo.py $< $@; \
	else touch $@; fi

$(BUILD)/wallpaper.raw: assets/wallpaper.png
	@mkdir -p $(BUILD)
	@if command -v python3 >/dev/null 2>&1 && [ -f tools/convert-wallpaper.py ]; then \
	    python3 tools/convert-wallpaper.py $< $@; \
	else touch $@; fi

# ���─ ISO construction ──────────────────────────���──────────────────────────────
GRUB_CFG ?= tools/grub.cfg
GRUB_FONT = $(BUILD)/grub-font.pf2

$(GRUB_FONT): assets/JetBrainsMono-Regular.ttf
	@mkdir -p $(BUILD)
	@if command -v grub-mkfont >/dev/null 2>&1; then \
	    grub-mkfont -o $@ -s 24 $<; \
	else touch $@; fi

$(BUILD)/aegis.iso: $(BUILD)/aegis.elf $(GRUB_CFG) $(ROOTFS) $(ESP_IMG) $(GRUB_FONT)
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/aegis.elf $(ISO_DIR)/boot/aegis.elf
	cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	@if [ -f assets/wallpaper.png ]; then cp assets/wallpaper.png $(ISO_DIR)/boot/grub/wallpaper.png; fi
	@if [ -s $(GRUB_FONT) ]; then cp $(GRUB_FONT) $(ISO_DIR)/boot/grub/font.pf2; fi
	cp $(ROOTFS) $(ISO_DIR)/boot/rootfs.img
	cp $(ESP_IMG) $(ISO_DIR)/boot/esp.img
	grub-mkrescue -o $@ $(ISO_DIR)

iso: $(BUILD)/aegis.iso

# Text-mode ISO — tools/grub-test.cfg boots `boot=text quiet` with
# timeout=0 so the kernel comes up on a TTY instead of Bastion's
# graphical greeter. Used by installer_test.rs (which needs to drive
# stsh via HMP sendkey) and any other test that prefers text mode.
TEST_ISO_DIR = $(BUILD)/test-isodir
$(BUILD)/aegis-test.iso: $(BUILD)/aegis.elf tools/grub-test.cfg $(ROOTFS) $(ESP_IMG) $(GRUB_FONT)
	@mkdir -p $(TEST_ISO_DIR)/boot/grub
	cp $(BUILD)/aegis.elf $(TEST_ISO_DIR)/boot/aegis.elf
	cp tools/grub-test.cfg $(TEST_ISO_DIR)/boot/grub/grub.cfg
	@if [ -s $(GRUB_FONT) ]; then cp $(GRUB_FONT) $(TEST_ISO_DIR)/boot/grub/font.pf2; fi
	cp $(ROOTFS) $(TEST_ISO_DIR)/boot/rootfs.img
	cp $(ESP_IMG) $(TEST_ISO_DIR)/boot/esp.img
	grub-mkrescue -o $@ $(TEST_ISO_DIR)

test-iso: $(BUILD)/aegis-test.iso

# ── Rootfs image (built by script, reads rootfs.manifest) ───────────────────
# Collect all source files mentioned in the manifest so Make knows to rebuild.
MANIFEST_SRCS := $(shell grep -v '^\#' rootfs.manifest 2>/dev/null | awk 'NF>=2 {print $$1}')

$(ROOTFS): $(MANIFEST_SRCS) $(BUILD)/aegis.elf $(BUILD)/wallpaper.raw $(BUILD)/logo.raw $(BUILD)/claude.raw
	bash tools/build-rootfs.sh $@ $(BUILD)/aegis.elf $(BUILD)/wallpaper.raw $(BUILD)/logo.raw $(BUILD)/claude.raw

rootfs: $(ROOTFS)

# ── Disk image (GPT wrapper around rootfs) ─────────���─────────────────────────
P1_SECTORS = 120832

$(DISK): $(ROOTFS)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=64 2>/dev/null
	$(SGDISK) \
	    --new=1:34:122879   --typecode=1:A3618F24-0C76-4B3D-0001-000000000000 --change-name=1:aegis-root \
	    --new=2:122880:0    --typecode=2:A3618F24-0C76-4B3D-0002-000000000000 --change-name=2:aegis-swap \
	    $(DISK)
	dd if=$(ROOTFS) of=$(DISK) bs=512 seek=2048 conv=notrunc 2>/dev/null
	@echo "Disk image created: $(DISK)"

disk: $(DISK)

# ── Run targets ───────────��───────────────────────────��──────────────────────
comma := ,
NVME_FLAGS = $(if $(wildcard $(DISK)),\
    -drive file=$(DISK)$(comma)if=none$(comma)id=nvme0 \
    -device nvme$(comma)drive=nvme0$(comma)serial=aegis0,)

run: iso
	qemu-system-x86_64 \
	    -machine q35 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga std -no-reboot -m 2G \
	    $(NVME_FLAGS) \
	    -device qemu-xhci -device usb-kbd \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

run-fb: iso
	qemu-system-x86_64 \
	    -machine q35 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga none -device virtio-vga -no-reboot -m 2G \
	    $(NVME_FLAGS) \
	    -device qemu-xhci -device usb-kbd \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

shell:
	$(MAKE) run

login:
	$(MAKE) run

# ── Debug targets ────────────────────────────────────────────────────────────
gdb: iso
	@echo "[GDB] QEMU GDB server on :1234 — serial -> $(BUILD)/debug.log"
	@qemu-system-x86_64 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial file:$(BUILD)/debug.log \
	    -vga std -no-reboot -m 2G \
	    -s -S \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -display none & \
	sleep 0.3 && gdb -x tools/aegis.gdb

sym:
	@addr2line -e $(BUILD)/aegis.elf -f -p $(ADDR)

# ── Tests ─────────────────────────────────────────────────────────────────────
test: iso
	cargo test --manifest-path tests/Cargo.toml -- --nocapture

test-q35: iso disk
	AEGIS_PRESET=q35 cargo test --manifest-path tests/Cargo.toml -- --nocapture

install-test: iso disk
	vortex stack up aegis-install-test

# ─��� Clean ────────���──────────────────────���────────────────────────────────────
clean:
	rm -rf $(BUILD)
	rm -f .init_stamp_*
	rm -f user/bin/rune
	@for d in user/bin/*/; do \
		[ -f "$$d/Makefile" ] && $(MAKE) -C "$$d" clean 2>/dev/null; \
	done; true
	@for d in user/lib/*/; do \
		[ -f "$$d/Makefile" ] && $(MAKE) -C "$$d" clean 2>/dev/null; \
	done; true
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
	$(CARGO) clean --manifest-path tests/Cargo.toml 2>/dev/null || rm -rf tests/target
