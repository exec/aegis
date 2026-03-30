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
DISK    = $(BUILD)/disk.img
ROOTFS  = $(BUILD)/rootfs.img
SGDISK  = /usr/sbin/sgdisk

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
else ifeq ($(INIT),login)
INIT_ELF_SRC = user/login/login.elf
else ifeq ($(INIT),vigil)
INIT_ELF_SRC = user/vigil/vigil
else
INIT_ELF_SRC = user/hello/hello.elf
endif

# Stamp file forces init blob rebuild when INIT= changes.
# Without this, 'make INIT=vigil iso' after 'make' (INIT=oksh) would
# silently keep the old init binary because init.o is already up-to-date.
INIT_STAMP = $(BUILD)/.init_stamp_$(INIT)
$(INIT_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.init_stamp_*
	@touch $@

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
    kernel/signal/signal.c

MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kva.c \
    kernel/mm/vma.c

BOOT_SRC = kernel/arch/x86_64/boot.asm
CAP_LIB  = kernel/cap/target/x86_64-unknown-none/release/libcap.a

ARCH_ASMS = \
    kernel/arch/x86_64/isr.asm \
    kernel/arch/x86_64/ctx_switch.asm \
    kernel/arch/x86_64/syscall_entry.asm \
    kernel/arch/x86_64/ap_trampoline.asm

SCHED_SRCS = kernel/sched/sched.c

DRIVER_SRCS = \
    kernel/drivers/nvme.c \
    kernel/drivers/xhci.c \
    kernel/drivers/usb_hid.c \
    kernel/drivers/usb_mouse.c \
    kernel/drivers/virtio_net.c \
    kernel/drivers/fb.c \
    kernel/drivers/ramdisk.c

NET_SRCS = \
    kernel/net/netdev.c \
    kernel/net/eth.c \
    kernel/net/ip.c \
    kernel/net/udp.c \
    kernel/net/tcp.c \
    kernel/net/socket.c \
    kernel/net/epoll.c

FS_SRCS = \
    kernel/fs/fd_table.c \
    kernel/fs/vfs.c \
    kernel/fs/initrd.c \
    kernel/fs/console.c \
    kernel/fs/kbd_vfs.c \
    kernel/fs/pipe.c \
    kernel/fs/blkdev.c \
    kernel/fs/gpt.c \
    kernel/fs/ext2.c \
    kernel/fs/ext2_cache.c \
    kernel/fs/ext2_dir.c \
    kernel/fs/ramfs.c \
    kernel/fs/procfs.c \
    kernel/fs/tty.c \
    kernel/fs/pty.c

USERSPACE_SRCS = \
    kernel/syscall/syscall.c \
    kernel/syscall/sys_io.c \
    kernel/syscall/sys_memory.c \
    kernel/syscall/sys_process.c \
    kernel/syscall/sys_file.c \
    kernel/syscall/sys_signal.c \
    kernel/syscall/sys_socket.c \
    kernel/syscall/sys_random.c \
    kernel/syscall/sys_disk.c \
    kernel/syscall/futex.c \
    kernel/proc/proc.c \
    kernel/elf/elf.c \

# Programs embedded in initrd via objcopy --input binary
OBJCOPY = x86_64-elf-objcopy

# Boot-critical static binaries in initrd: login, vigil, shell, echo, cat, ls.
# echo/cat/ls are needed by INIT=shell tests (test_ext2, test_gpt).
# All other user binaries are dynamically linked and live on the ext2 disk.
USER_ELFS = \
    user/login/login.elf \
    user/vigil/vigil \
    user/shell/shell.elf

BLOB_OBJS = $(BUILD)/blobs/login.o $(BUILD)/blobs/vigil.o $(BUILD)/blobs/shell.o \
            $(BUILD)/blobs/echo.o $(BUILD)/blobs/cat.o $(BUILD)/blobs/ls.o \
            $(BUILD)/blobs/init.o


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

ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) $(MM_OBJS) \
           $(SCHED_OBJS) $(FS_OBJS) $(DRIVER_OBJS) $(NET_OBJS) $(USERSPACE_OBJS) $(BLOB_OBJS)

.PHONY: all iso disk run run-fb shell oksh login test clean gdb sym curl_bin build-musl \
        user/vigil/vigil user/login/login.elf user/vigictl/vigictl user/stsh/stsh.elf

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
# ── Program ELF binaries ──────────────────────────────────────────────────────
# Dynamic user binaries depend on musl shared build (for musl-gcc wrapper)
MUSL_BUILT = build/musl-dynamic/usr/lib/libc.so

user/ls/ls.elf: $(MUSL_BUILT)
	$(MAKE) -C user/ls

user/cat/cat.elf: $(MUSL_BUILT)
	$(MAKE) -C user/cat

user/echo/echo.elf: $(MUSL_BUILT)
	$(MAKE) -C user/echo

user/pwd/pwd.elf: $(MUSL_BUILT)
	$(MAKE) -C user/pwd

user/uname/uname.elf: $(MUSL_BUILT)
	$(MAKE) -C user/uname

user/clear/clear.elf: $(MUSL_BUILT)
	$(MAKE) -C user/clear

user/true/true.elf: $(MUSL_BUILT)
	$(MAKE) -C user/true

user/false/false.elf: $(MUSL_BUILT)
	$(MAKE) -C user/false

user/wc/wc.elf: $(MUSL_BUILT)
	$(MAKE) -C user/wc

user/grep/grep.elf: $(MUSL_BUILT)
	$(MAKE) -C user/grep

user/sort/sort.elf: $(MUSL_BUILT)
	$(MAKE) -C user/sort

user/mkdir/mkdir.elf: $(MUSL_BUILT)
	$(MAKE) -C user/mkdir

user/touch/touch.elf: $(MUSL_BUILT)
	$(MAKE) -C user/touch

user/rm/rm.elf: $(MUSL_BUILT)
	$(MAKE) -C user/rm

user/cp/cp.elf: $(MUSL_BUILT)
	$(MAKE) -C user/cp

user/mv/mv.elf: $(MUSL_BUILT)
	$(MAKE) -C user/mv

user/whoami/whoami.elf: $(MUSL_BUILT)
	$(MAKE) -C user/whoami

user/ln/ln.elf: $(MUSL_BUILT)
	$(MAKE) -C user/ln

user/chmod/chmod.elf: $(MUSL_BUILT)
	$(MAKE) -C user/chmod

user/chown/chown.elf: $(MUSL_BUILT)
	$(MAKE) -C user/chown

user/readlink/readlink.elf: $(MUSL_BUILT)
	$(MAKE) -C user/readlink

user/stsh/stsh.elf: $(MUSL_BUILT)
	$(MAKE) -C user/stsh

# Static binaries — no musl dependency
user/shell/shell.elf:
	$(MAKE) -C user/shell

user/oksh/oksh.elf: $(MUSL_BUILT)
	$(MAKE) -C user/oksh

user/login/login.elf: $(MUSL_BUILT)
	$(MAKE) -C user/login

user/vigil/vigil: user/vigil/main.c $(MUSL_BUILT)
	$(MAKE) -C user/vigil

user/vigictl/vigictl: user/vigictl/main.c $(MUSL_BUILT)
	$(MAKE) -C user/vigictl

user/httpd/httpd.elf: $(MUSL_BUILT)
	$(MAKE) -C user/httpd

user/dhcp/dhcp: user/dhcp/main.c $(MUSL_BUILT)
	$(MAKE) -C user/dhcp

user/chronos/chronos: user/chronos/main.c $(MUSL_BUILT)
	$(MAKE) -C user/chronos

user/thread_test/thread_test.elf: user/thread_test/main.c $(MUSL_BUILT)
	$(MAKE) -C user/thread_test

user/mmap_test/mmap_test.elf: user/mmap_test/main.c $(MUSL_BUILT)
	$(MAKE) -C user/mmap_test

user/proc_test/proc_test.elf: user/proc_test/main.c $(MUSL_BUILT)
	$(MAKE) -C user/proc_test

user/pty_test/pty_test.elf: user/pty_test/main.c $(MUSL_BUILT)
	$(MAKE) -C user/pty_test

# ── Binary blob embedding (objcopy) ──────────────────────────────────────────
# Each user ELF → linkable .o with _binary_<name>_bin_start/end symbols.
# Copy to build/blobs/<name>.bin, cd there, objcopy — so symbol names are clean.

$(BUILD)/blobs/login.o: user/login/login.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/login.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  login.bin login.o

$(BUILD)/blobs/vigil.o: user/vigil/vigil
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/vigil.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  vigil.bin vigil.o

$(BUILD)/blobs/shell.o: user/shell/shell.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/shell.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  shell.bin shell.o

# Static initrd copies of echo/cat/ls — compiled directly, NOT from user/*/Makefile
# (those produce dynamic binaries for ext2). These are only used by INIT=shell tests.
$(BUILD)/blobs/echo.o: user/echo/main.c
	@mkdir -p $(BUILD)/blobs
	musl-gcc -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none -o $(BUILD)/blobs/echo.bin $<
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  echo.bin echo.o

$(BUILD)/blobs/cat.o: user/cat/main.c
	@mkdir -p $(BUILD)/blobs
	musl-gcc -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none -o $(BUILD)/blobs/cat.bin $<
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  cat.bin cat.o

$(BUILD)/blobs/ls.o: user/ls/main.c
	@mkdir -p $(BUILD)/blobs
	musl-gcc -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none -o $(BUILD)/blobs/ls.bin $<
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  ls.bin ls.o

$(BUILD)/blobs/init.o: $(INIT_ELF_SRC) $(INIT_STAMP)
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/init.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  init.bin init.o

# BearSSL source (fetch if absent)
references/bearssl-0.6/inc/bearssl.h:
	bash tools/fetch-bearssl.sh

# BearSSL staging install
build/bearssl-install/lib/libbearssl.a: references/bearssl-0.6/inc/bearssl.h
	bash tools/build-bearssl.sh

# curl binary
build/curl/curl: build/bearssl-install/lib/libbearssl.a
	bash tools/build-curl.sh

curl_bin: build/curl/curl

# musl shared library (dynamic linker)
build/musl-dynamic/usr/lib/libc.so:
	bash tools/build-musl.sh

build-musl: build/musl-dynamic/usr/lib/libc.so

user/dynlink_test/dynlink_test.elf: user/dynlink_test/main.c build/musl-dynamic/usr/lib/libc.so
	$(MAKE) -C user/dynlink_test

user/installer/installer.elf: user/installer/main.c $(MUSL_BUILT)
	$(MAKE) -C user/installer

user/fb_test/fb_test.elf: user/fb_test/main.c user/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/fb_test

user/mouse_test/mouse_test.elf: user/mouse_test/main.c $(MUSL_BUILT)
	$(MAKE) -C user/mouse_test

user/glyph/libglyph.a: $(wildcard user/glyph/*.c user/glyph/*.h) $(MUSL_BUILT)
	$(MAKE) -C user/glyph

user/lumen/lumen.elf: $(wildcard user/lumen/*.c user/lumen/*.h) user/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/lumen

# ── EFI GRUB + ESP image for installer ─────────────────────────────────────
# BOOTX64.EFI: UEFI GRUB binary (x86_64-efi) with ext2 + multiboot2 support
# esp.img: pre-built FAT16 EFI System Partition image with BOOTX64.EFI + grub.cfg
GRUB_EFI = $(BUILD)/BOOTX64.EFI
ESP_IMG  = $(BUILD)/esp.img

$(GRUB_EFI):
	@mkdir -p $(BUILD)
	grub-mkimage -O x86_64-efi -o $(GRUB_EFI) \
	    -p /EFI/BOOT \
	    part_gpt ext2 fat normal multiboot2 boot configfile \
	    all_video efi_gop efi_uga video video_bochs video_cirrus gfxterm \
	    font gfxmenu jpeg png

$(ESP_IMG): $(GRUB_EFI)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(ESP_IMG) bs=512 count=65536 2>/dev/null
	/sbin/mkfs.fat -F 16 $(ESP_IMG) >/dev/null 2>&1
	mmd -i $(ESP_IMG) ::EFI
	mmd -i $(ESP_IMG) ::EFI/BOOT
	mcopy -i $(ESP_IMG) $(GRUB_EFI) ::EFI/BOOT/BOOTX64.EFI
	mcopy -i $(ESP_IMG) /usr/share/grub/unicode.pf2 ::EFI/BOOT/unicode.pf2
	mcopy -i $(ESP_IMG) tools/grub-bg.jpg ::EFI/BOOT/bg.jpg
	@printf 'insmod all_video\ninsmod gfxterm\ninsmod jpeg\nset gfxmode=auto\nterminal_input console\nterminal_output gfxterm\nloadfont $$prefix/unicode.pf2\nbackground_image $$prefix/bg.jpg\nset menu_color_normal=light-green/black\nset menu_color_highlight=white/dark-gray\nset color_normal=light-green/black\nset color_highlight=white/dark-gray\nset timeout=3\nset default=0\n\nmenuentry "Aegis" {\n    set gfxpayload=keep\n    set root=(hd0,gpt2)\n    multiboot2 /boot/aegis.elf\n    boot\n}\n' > $(BUILD)/grub-installed.cfg
	mcopy -i $(ESP_IMG) $(BUILD)/grub-installed.cfg ::EFI/BOOT/grub.cfg

# ── Final link ────────────────────────────────────────────────────────────────
$(BUILD)/aegis.elf: $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)

$(BUILD)/aegis.iso: $(BUILD)/aegis.elf tools/grub.cfg $(ROOTFS) $(ESP_IMG)
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/aegis.elf $(ISO_DIR)/boot/aegis.elf
	cp tools/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	cp $(ROOTFS) $(ISO_DIR)/boot/rootfs.img
	cp $(ESP_IMG) $(ISO_DIR)/boot/esp.img
	grub-mkrescue -o $@ $(ISO_DIR)

# ── Run targets ───────────────────────────────────────────────────────────────
iso: $(BUILD)/aegis.iso

# nvme0p1: sgdisk aligns start to LBA 2048, end 122879 = 120832 sectors = ~59 MB
P1_SECTORS = 120832

# All user ELFs required before disk can be populated.
# debugfs silently skips `write` commands whose source file is missing,
# producing a disk with an empty /bin and no build error — so we list
# them all as prerequisites here.
DISK_USER_BINS = \
	user/login/login.elf user/vigil/vigil \
	user/shell/shell.elf user/ls/ls.elf user/cat/cat.elf \
	user/echo/echo.elf user/pwd/pwd.elf user/uname/uname.elf \
	user/clear/clear.elf user/true/true.elf user/false/false.elf \
	user/wc/wc.elf user/grep/grep.elf user/sort/sort.elf \
	user/mv/mv.elf user/cp/cp.elf user/rm/rm.elf \
	user/mkdir/mkdir.elf user/touch/touch.elf \
	user/whoami/whoami.elf \
	user/ln/ln.elf user/chmod/chmod.elf \
	user/chown/chown.elf user/readlink/readlink.elf \
	user/stsh/stsh.elf \
	user/oksh/oksh.elf \
	user/httpd/httpd.elf \
	user/vigictl/vigictl \
	user/thread_test/thread_test.elf \
	user/mmap_test/mmap_test.elf \
	user/proc_test/proc_test.elf \
	user/pty_test/pty_test.elf \
	user/dynlink_test/dynlink_test.elf \
	user/dhcp/dhcp \
	user/installer/installer.elf \
	user/fb_test/fb_test.elf \
	user/mouse_test/mouse_test.elf \
	user/lumen/lumen.elf \
	user/chronos/chronos \
	build/curl/curl \
	build/musl-dynamic/usr/lib/libc.so

rootfs: $(ROOTFS)
disk: $(DISK)

# ── Wallpaper conversion (PNG → raw BGRA) ──
WALLPAPER_SRC ?= assets/wallpaper.png
$(BUILD)/wallpaper.raw:
	@mkdir -p $(BUILD)
	@if [ -f $(WALLPAPER_SRC) ] && command -v python3 >/dev/null 2>&1; then \
	    python3 tools/convert-wallpaper.py $(WALLPAPER_SRC) $@; \
	else \
	    echo "Note: wallpaper source not found — skipping"; \
	    touch $@; \
	fi

# ── rootfs.img: standalone ext2 filesystem image (embedded in ISO as module) ──
# ALWAYS rebuild rootfs — stale rootfs.img inside the ISO caused the
# 2026-03-28 debugging catastrophe. Deleting it first ensures every
# user binary is freshly written even on incremental builds.
$(ROOTFS): $(DISK_USER_BINS) $(BUILD)/aegis.elf $(BUILD)/wallpaper.raw
	@rm -f $(ROOTFS)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(ROOTFS) bs=512 count=$(P1_SECTORS) 2>/dev/null
	/sbin/mke2fs -t ext2 -F -L aegis-root $(ROOTFS)
	printf 'mkdir /bin\nmkdir /etc\nmkdir /tmp\nmkdir /home\nmkdir /lib\nmkdir /root\nmkdir /proc\nmkdir /dev\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	@printf '\n _______ _______  ______ _____ _______\n |_____| |______ |  ____   |   |______\n |     | |______ |_____| __|__ ______|\n\n Aegis 1.0.0 "Ambient Argus"\n\n' > /tmp/aegis-motd
	# Dynamic linker / shared library — written as two separate files
	# (ext2 now supports symlinks but debugfs symlink cmd is inconsistent)
	printf 'write build/musl-dynamic/usr/lib/libc.so /lib/libc.so\nwrite build/musl-dynamic/usr/lib/libc.so /lib/ld-musl-x86_64.so.1\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	# User binaries (dynamically linked, loaded from ext2 at runtime)
	printf 'write user/shell/shell.elf /bin/sh\nwrite user/ls/ls.elf /bin/ls\nwrite user/cat/cat.elf /bin/cat\nwrite user/echo/echo.elf /bin/echo\nwrite user/pwd/pwd.elf /bin/pwd\nwrite user/uname/uname.elf /bin/uname\nwrite user/clear/clear.elf /bin/clear\nwrite user/true/true.elf /bin/true\nwrite user/false/false.elf /bin/false\nwrite user/wc/wc.elf /bin/wc\nwrite user/grep/grep.elf /bin/grep\nwrite user/sort/sort.elf /bin/sort\nwrite user/mv/mv.elf /bin/mv\nwrite user/cp/cp.elf /bin/cp\nwrite user/rm/rm.elf /bin/rm\nwrite user/mkdir/mkdir.elf /bin/mkdir\nwrite user/touch/touch.elf /bin/touch\nwrite user/whoami/whoami.elf /bin/whoami\nwrite user/oksh/oksh.elf /bin/oksh\nwrite user/httpd/httpd.elf /bin/httpd\nwrite user/vigictl/vigictl /bin/vigictl\nwrite user/thread_test/thread_test.elf /bin/thread_test\nwrite user/mmap_test/mmap_test.elf /bin/mmap_test\nwrite user/proc_test/proc_test.elf /bin/proc_test\nwrite user/pty_test/pty_test.elf /bin/pty_test\nwrite user/dhcp/dhcp /bin/dhcp\nwrite user/dynlink_test/dynlink_test.elf /bin/dynlink_test\nwrite user/vigil/vigil /bin/vigil\nwrite user/login/login.elf /bin/login\nwrite user/installer/installer.elf /bin/installer\nwrite user/fb_test/fb_test.elf /bin/fb_test\nwrite user/mouse_test/mouse_test.elf /bin/mouse_test\nwrite user/lumen/lumen.elf /bin/lumen\nwrite user/chronos/chronos /bin/chronos\nwrite user/ln/ln.elf /bin/ln\nwrite user/chmod/chmod.elf /bin/chmod\nwrite user/chown/chown.elf /bin/chown\nwrite user/readlink/readlink.elf /bin/readlink\nwrite user/stsh/stsh.elf /bin/stsh\nwrite /tmp/aegis-motd /etc/motd\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	# Set execute permission on all binaries (debugfs writes mode 0644 by default)
	printf 'set_inode_field /bin/sh mode 0100755\nset_inode_field /bin/ls mode 0100755\nset_inode_field /bin/cat mode 0100755\nset_inode_field /bin/echo mode 0100755\nset_inode_field /bin/pwd mode 0100755\nset_inode_field /bin/uname mode 0100755\nset_inode_field /bin/clear mode 0100755\nset_inode_field /bin/true mode 0100755\nset_inode_field /bin/false mode 0100755\nset_inode_field /bin/wc mode 0100755\nset_inode_field /bin/grep mode 0100755\nset_inode_field /bin/sort mode 0100755\nset_inode_field /bin/mv mode 0100755\nset_inode_field /bin/cp mode 0100755\nset_inode_field /bin/rm mode 0100755\nset_inode_field /bin/mkdir mode 0100755\nset_inode_field /bin/touch mode 0100755\nset_inode_field /bin/whoami mode 0100755\nset_inode_field /bin/oksh mode 0100755\nset_inode_field /bin/httpd mode 0100755\nset_inode_field /bin/vigictl mode 0100755\nset_inode_field /bin/thread_test mode 0100755\nset_inode_field /bin/mmap_test mode 0100755\nset_inode_field /bin/proc_test mode 0100755\nset_inode_field /bin/pty_test mode 0100755\nset_inode_field /bin/dhcp mode 0100755\nset_inode_field /bin/dynlink_test mode 0100755\nset_inode_field /bin/vigil mode 0100755\nset_inode_field /bin/login mode 0100755\nset_inode_field /bin/installer mode 0100755\nset_inode_field /bin/fb_test mode 0100755\nset_inode_field /bin/mouse_test mode 0100755\nset_inode_field /bin/lumen mode 0100755\nset_inode_field /bin/chronos mode 0100755\nset_inode_field /bin/ln mode 0100755\nset_inode_field /bin/chmod mode 0100755\nset_inode_field /bin/chown mode 0100755\nset_inode_field /bin/readlink mode 0100755\nset_inode_field /bin/stsh mode 0100755\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	# Auth files for login
	printf 'root:x:0:0:root:/root:/bin/stsh\n' > /tmp/aegis-passwd
	printf 'root:$$6$$5a3b9c1d2e4f6789$$fvwyIjdmyvB59hifGMRFrcwhBb4cH0.3nRy2j2LpCk.aNIFNyvYQJ36Bsl94miFbD/JHICz8O1dXoegZ0OmOg.:19000:0:99999:7:::\n' > /tmp/aegis-shadow
	printf 'root:x:0:root\nwheel:x:999:root\n' > /tmp/aegis-group
	printf 'write /tmp/aegis-passwd /etc/passwd\nwrite /tmp/aegis-shadow /etc/shadow\nwrite /tmp/aegis-group /etc/group\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	rm -f /tmp/aegis-passwd /tmp/aegis-shadow /tmp/aegis-group
	# /etc/hosts and /etc/profile for writable root
	printf '127.0.0.1 localhost\n10.0.2.15 aegis\n104.18.27.120 example.com\n' > /tmp/aegis-hosts
	printf "PS1='root@aegis:\$${PWD:-/}# '\nexport PS1\nPATH=/bin\nexport PATH\n" > /tmp/aegis-profile
	@printf '\n _______ _______  ______ _____ _______\n |_____| |______ |  ____   |   |______\n |     | |______ |_____| __|__ ______|\n\n WARNING: This system is restricted to authorized users.\n All activity is monitored and logged. Unauthorized access\n will be investigated and may result in prosecution.\n\n' > /tmp/aegis-banner
	@printf '\n _______ _______  ______ _____ _______\n |_____| |______ |  ____   |   |______\n |     | |______ |_____| __|__ ______|\n\n WARNING: This system is restricted to authorized users.\n All connections are monitored and logged.\n\n' > /tmp/aegis-banner-net
	printf 'write /tmp/aegis-hosts /etc/hosts\nwrite /tmp/aegis-profile /etc/profile\nwrite /tmp/aegis-banner /etc/banner\nwrite /tmp/aegis-banner-net /etc/banner.net\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	rm -f /tmp/aegis-hosts /tmp/aegis-profile /tmp/aegis-banner /tmp/aegis-banner-net
	# Vigil init directories and service declarations
	printf 'mkdir /var\nmkdir /run\nmkdir /etc/vigil\nmkdir /etc/vigil/services\nmkdir /etc/vigil/services/getty\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf '/bin/login\n' > /tmp/aegis-vigil-run
	printf 'respawn\nmax_restarts=5\n' > /tmp/aegis-vigil-policy
	printf 'VFS_OPEN VFS_READ VFS_WRITE AUTH CAP_GRANT CAP_DELEGATE CAP_QUERY\n' > /tmp/aegis-vigil-caps
	printf 'root\n' > /tmp/aegis-vigil-user
	printf 'text\n' > /tmp/aegis-vigil-mode
	printf 'write /tmp/aegis-vigil-run /etc/vigil/services/getty/run\nwrite /tmp/aegis-vigil-policy /etc/vigil/services/getty/policy\nwrite /tmp/aegis-vigil-caps /etc/vigil/services/getty/caps\nwrite /tmp/aegis-vigil-user /etc/vigil/services/getty/user\nwrite /tmp/aegis-vigil-mode /etc/vigil/services/getty/mode\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	rm -f /tmp/aegis-vigil-run /tmp/aegis-vigil-policy /tmp/aegis-vigil-caps /tmp/aegis-vigil-user /tmp/aegis-vigil-mode
	# lumen vigil service — graphical compositor (graphical mode only)
	printf 'mkdir /etc/vigil/services/lumen\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf '/bin/lumen\n' > /tmp/aegis-lumen-run
	printf 'respawn\nmax_restarts=3\n' > /tmp/aegis-lumen-policy
	printf 'FB VFS_OPEN VFS_READ VFS_WRITE\n' > /tmp/aegis-lumen-caps
	printf 'root\n' > /tmp/aegis-lumen-user
	printf 'graphical\n' > /tmp/aegis-lumen-mode
	printf 'write /tmp/aegis-lumen-run /etc/vigil/services/lumen/run\nwrite /tmp/aegis-lumen-policy /etc/vigil/services/lumen/policy\nwrite /tmp/aegis-lumen-caps /etc/vigil/services/lumen/caps\nwrite /tmp/aegis-lumen-user /etc/vigil/services/lumen/user\nwrite /tmp/aegis-lumen-mode /etc/vigil/services/lumen/mode\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	rm -f /tmp/aegis-lumen-run /tmp/aegis-lumen-policy /tmp/aegis-lumen-caps /tmp/aegis-lumen-user /tmp/aegis-lumen-mode
	# httpd vigil service — binds :80, serves HTTP
	printf 'mkdir /etc/vigil/services/httpd\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf '/bin/httpd\n' > /tmp/aegis-httpd-run
	printf 'respawn\nmax_restarts=5\n' > /tmp/aegis-httpd-policy
	printf 'NET_SOCKET VFS_OPEN VFS_READ\n' > /tmp/aegis-httpd-caps
	printf 'root\n' > /tmp/aegis-httpd-user
	printf 'write /tmp/aegis-httpd-run /etc/vigil/services/httpd/run\nwrite /tmp/aegis-httpd-policy /etc/vigil/services/httpd/policy\nwrite /tmp/aegis-httpd-caps /etc/vigil/services/httpd/caps\nwrite /tmp/aegis-httpd-user /etc/vigil/services/httpd/user\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	rm -f /tmp/aegis-httpd-run /tmp/aegis-httpd-policy /tmp/aegis-httpd-caps /tmp/aegis-httpd-user
	# dhcp vigil service — DHCP client with NET_ADMIN cap
	printf 'mkdir /etc/vigil/services/dhcp\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf '/bin/dhcp\n' > /tmp/aegis-dhcp-run
	printf 'oneshot\n' > /tmp/aegis-dhcp-policy
	printf 'NET_ADMIN NET_SOCKET\n' > /tmp/aegis-dhcp-caps
	printf 'root\n' > /tmp/aegis-dhcp-user
	printf 'write /tmp/aegis-dhcp-run /etc/vigil/services/dhcp/run\nwrite /tmp/aegis-dhcp-policy /etc/vigil/services/dhcp/policy\nwrite /tmp/aegis-dhcp-caps /etc/vigil/services/dhcp/caps\nwrite /tmp/aegis-dhcp-user /etc/vigil/services/dhcp/user\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	rm -f /tmp/aegis-dhcp-run /tmp/aegis-dhcp-policy /tmp/aegis-dhcp-caps /tmp/aegis-dhcp-user
	# chronos vigil service — NTP time sync daemon
	printf 'mkdir /etc/vigil/services/chronos\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf '/bin/chronos\n' > /tmp/aegis-chronos-run
	printf 'oneshot\n' > /tmp/aegis-chronos-policy
	printf 'NET_SOCKET\n' > /tmp/aegis-chronos-caps
	printf 'root\n' > /tmp/aegis-chronos-user
	printf 'write /tmp/aegis-chronos-run /etc/vigil/services/chronos/run\nwrite /tmp/aegis-chronos-policy /etc/vigil/services/chronos/policy\nwrite /tmp/aegis-chronos-caps /etc/vigil/services/chronos/caps\nwrite /tmp/aegis-chronos-user /etc/vigil/services/chronos/user\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	rm -f /tmp/aegis-chronos-run /tmp/aegis-chronos-policy /tmp/aegis-chronos-caps /tmp/aegis-chronos-user
	# curl binary and CA bundle on ext2
	printf 'mkdir /etc/ssl\nmkdir /etc/ssl/certs\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf 'write build/curl/curl /bin/curl\nwrite tools/cacert.pem /etc/ssl/certs/ca-certificates.crt\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	# Desktop wallpaper (converted from PNG at build time)
	@if [ -f $(BUILD)/wallpaper.raw ]; then \
	    printf 'mkdir /usr\nmkdir /usr/share\n' | /sbin/debugfs -w $(ROOTFS); \
	    printf 'write $(BUILD)/wallpaper.raw /usr/share/wallpaper.raw\n' | /sbin/debugfs -w $(ROOTFS); \
	fi
	# TTF fonts for Lumen compositor (optional — UI falls back to bitmap font)
	@if [ -f assets/Inter-Regular.ttf ] || [ -f assets/JetBrainsMono-Regular.ttf ]; then \
	    printf 'mkdir /usr\nmkdir /usr/share\nmkdir /usr/share/fonts\n' | /sbin/debugfs -w $(ROOTFS); \
	    if [ -f assets/Inter-Regular.ttf ]; then \
	        printf 'write assets/Inter-Regular.ttf /usr/share/fonts/Inter-Regular.ttf\n' | /sbin/debugfs -w $(ROOTFS); \
	    fi; \
	    if [ -f assets/JetBrainsMono-Regular.ttf ]; then \
	        printf 'write assets/JetBrainsMono-Regular.ttf /usr/share/fonts/JetBrainsMono-Regular.ttf\n' | /sbin/debugfs -w $(ROOTFS); \
	    fi; \
	fi
	# Kernel binary for installed-system boot
	printf 'mkdir /boot\nmkdir /boot/grub\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf 'write $(BUILD)/aegis.elf /boot/aegis.elf\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	@rm -f /tmp/aegis-motd
	@echo "Root filesystem image created: $(ROOTFS)"

# ── disk.img: GPT disk wrapping rootfs.img (for NVMe testing) ──
$(DISK): $(ROOTFS)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=64 2>/dev/null
	$(SGDISK) \
	    --new=1:34:122879   --typecode=1:A3618F24-0C76-4B3D-0001-000000000000 --change-name=1:aegis-root \
	    --new=2:122880:0    --typecode=2:A3618F24-0C76-4B3D-0002-000000000000 --change-name=2:aegis-swap \
	    $(DISK)
	dd if=$(ROOTFS) of=$(DISK) bs=512 seek=2048 conv=notrunc 2>/dev/null
	@echo "Disk image created: $(DISK)"

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

# run-fb: boot with virtio-vga display (framebuffer) instead of legacy VGA.
# Uses USB keyboard (-device usb-kbd) because virtio-vga replaces the legacy
# VGA device that normally coexists with the i8042 PS/2 controller; the USB
# HID path (xHCI → usb-kbd → kbd_usb_inject) is always reliable here.
# Do NOT combine with -vga std: two VGA adapters confuse GRUB's video init.
run-fb: iso
	qemu-system-x86_64 \
	    -machine q35 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga none -device virtio-vga -no-reboot -m 2G \
	    $(NVME_FLAGS) \
	    -device qemu-xhci -device usb-kbd \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

shell:
	$(MAKE) INIT=shell run

oksh:
	$(MAKE) INIT=oksh run

login:
	$(MAKE) INIT=login run

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
	    -vga std -no-reboot -m 2G \
	    -s -S \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -display none & \
	sleep 0.3 && gdb -x tools/aegis.gdb

sym:
	@addr2line -e $(BUILD)/aegis.elf -f -p $(ADDR)

test:
	$(MAKE) INIT=shell iso
	@cp $(BUILD)/aegis.iso $(BUILD)/aegis-test.iso
	$(MAKE) INIT=vigil iso
	$(MAKE) disk
	@bash tests/run_tests.sh

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
	rm -f .init_stamp_*
	@for d in user/*/; do \
		[ -f "$$d/Makefile" ] && $(MAKE) -C "$$d" clean 2>/dev/null; \
	done; true
	rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf
	rm -f user/dhcp/dhcp user/chronos/chronos user/lumen/lumen
	rm -f user/httpd/httpd user/installer/installer
	rm -f user/fb_test/fb_test user/mouse_test/mouse_test
	rm -f user/glyph/libglyph.a
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
