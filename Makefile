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

USERSPACE_SRCS = \
    kernel/syscall/syscall.c \
    kernel/syscall/sys_io.c \
    kernel/syscall/sys_memory.c \
    kernel/syscall/sys_process.c \
    kernel/syscall/sys_file.c \
    kernel/syscall/sys_signal.c \
    kernel/syscall/sys_socket.c \
    kernel/syscall/sys_random.c \
    kernel/syscall/futex.c \
    kernel/proc/proc.c \
    kernel/elf/elf.c \

# Programs embedded in initrd via objcopy --input binary
OBJCOPY = x86_64-elf-objcopy

USER_ELFS = \
    user/shell/shell.elf \
    user/ls/ls.elf \
    user/cat/cat.elf \
    user/echo/echo.elf \
    user/pwd/pwd.elf \
    user/uname/uname.elf \
    user/clear/clear.elf \
    user/true/true.elf \
    user/false/false.elf \
    user/wc/wc.elf \
    user/grep/grep.elf \
    user/sort/sort.elf \
    user/mkdir/mkdir.elf \
    user/touch/touch.elf \
    user/rm/rm.elf \
    user/cp/cp.elf \
    user/mv/mv.elf \
    user/whoami/whoami.elf \
    user/oksh/oksh.elf \
    user/login/login.elf \
    user/vigil/vigil \
    user/vigictl/vigictl \
    user/httpd/httpd.elf \
    user/dhcp/dhcp

BLOB_OBJS = $(patsubst %,$(BUILD)/blobs/%.o,$(notdir $(basename $(USER_ELFS)))) $(BUILD)/blobs/init.o


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

.PHONY: all iso disk run run-fb shell oksh login test clean gdb sym curl_bin

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

user/shell/shell.elf:
	$(MAKE) -C user/shell

user/oksh/oksh.elf:
	$(MAKE) -C user/oksh

user/login/login.elf:
	$(MAKE) -C user/login

user/vigil/vigil: user/vigil/main.c
	$(MAKE) -C user/vigil

user/vigictl/vigictl: user/vigictl/main.c
	$(MAKE) -C user/vigictl

user/httpd/httpd.elf:
	$(MAKE) -C user/httpd

user/dhcp/dhcp: user/dhcp/main.c
	$(MAKE) -C user/dhcp

# ── Binary blob embedding (objcopy) ──────────────────────────────────────────
# Each user ELF → linkable .o with _binary_<name>_bin_start/end symbols.
# Copy to build/blobs/<name>.bin, cd there, objcopy — so symbol names are clean.

$(BUILD)/blobs/shell.o: user/shell/shell.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/shell.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  shell.bin shell.o

$(BUILD)/blobs/ls.o: user/ls/ls.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/ls.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  ls.bin ls.o

$(BUILD)/blobs/cat.o: user/cat/cat.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/cat.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  cat.bin cat.o

$(BUILD)/blobs/echo.o: user/echo/echo.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/echo.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  echo.bin echo.o

$(BUILD)/blobs/pwd.o: user/pwd/pwd.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/pwd.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  pwd.bin pwd.o

$(BUILD)/blobs/uname.o: user/uname/uname.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/uname.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  uname.bin uname.o

$(BUILD)/blobs/clear.o: user/clear/clear.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/clear.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  clear.bin clear.o

$(BUILD)/blobs/true.o: user/true/true.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/true.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  true.bin true.o

$(BUILD)/blobs/false.o: user/false/false.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/false.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  false.bin false.o

$(BUILD)/blobs/wc.o: user/wc/wc.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/wc.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  wc.bin wc.o

$(BUILD)/blobs/grep.o: user/grep/grep.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/grep.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  grep.bin grep.o

$(BUILD)/blobs/sort.o: user/sort/sort.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/sort.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  sort.bin sort.o

$(BUILD)/blobs/mkdir.o: user/mkdir/mkdir.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/mkdir.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  mkdir.bin mkdir.o

$(BUILD)/blobs/touch.o: user/touch/touch.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/touch.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  touch.bin touch.o

$(BUILD)/blobs/rm.o: user/rm/rm.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/rm.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  rm.bin rm.o

$(BUILD)/blobs/cp.o: user/cp/cp.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/cp.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  cp.bin cp.o

$(BUILD)/blobs/mv.o: user/mv/mv.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/mv.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  mv.bin mv.o

$(BUILD)/blobs/whoami.o: user/whoami/whoami.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/whoami.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  whoami.bin whoami.o

$(BUILD)/blobs/oksh.o: user/oksh/oksh.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/oksh.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  oksh.bin oksh.o

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

$(BUILD)/blobs/vigictl.o: user/vigictl/vigictl
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/vigictl.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  vigictl.bin vigictl.o

$(BUILD)/blobs/httpd.o: user/httpd/httpd.elf
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/httpd.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  httpd.bin httpd.o

$(BUILD)/blobs/dhcp.o: user/dhcp/dhcp
	@mkdir -p $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/dhcp.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  dhcp.bin dhcp.o

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

# ── Final link ────────────────────────────────────────────────────────────────
$(BUILD)/aegis.elf: $(ALL_OBJS) $(CAP_LIB)
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
	user/mkdir/mkdir.elf user/touch/touch.elf \
	user/httpd/httpd.elf \
	user/dhcp/dhcp \
	build/curl/curl

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
	printf 'write user/shell/shell.elf /bin/sh\nwrite user/ls/ls.elf /bin/ls\nwrite user/cat/cat.elf /bin/cat\nwrite user/echo/echo.elf /bin/echo\nwrite user/pwd/pwd.elf /bin/pwd\nwrite user/uname/uname.elf /bin/uname\nwrite user/clear/clear.elf /bin/clear\nwrite user/true/true.elf /bin/true\nwrite user/false/false.elf /bin/false\nwrite user/wc/wc.elf /bin/wc\nwrite user/grep/grep.elf /bin/grep\nwrite user/sort/sort.elf /bin/sort\nwrite user/mv/mv.elf /bin/mv\nwrite user/cp/cp.elf /bin/cp\nwrite user/rm/rm.elf /bin/rm\nwrite user/mkdir/mkdir.elf /bin/mkdir\nwrite user/touch/touch.elf /bin/touch\nwrite user/httpd/httpd.elf /bin/httpd\nwrite user/thread_test/thread_test.elf /bin/thread_test\nwrite /tmp/aegis-motd /etc/motd\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	# Auth files for login
	printf 'root:x:0:0:root:/root:/bin/oksh\n' > /tmp/aegis-passwd
	printf 'root:$$6$$5a3b9c1d2e4f6789$$fvwyIjdmyvB59hifGMRFrcwhBb4cH0.3nRy2j2LpCk.aNIFNyvYQJ36Bsl94miFbD/JHICz8O1dXoegZ0OmOg.:19000:0:99999:7:::\n' > /tmp/aegis-shadow
	printf 'root:x:0:root\nwheel:x:999:root\n' > /tmp/aegis-group
	printf 'write /tmp/aegis-passwd /etc/passwd\nwrite /tmp/aegis-shadow /etc/shadow\nwrite /tmp/aegis-group /etc/group\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	rm -f /tmp/aegis-passwd /tmp/aegis-shadow /tmp/aegis-group
	# Vigil init directories and service declarations
	printf 'mkdir /var\nmkdir /run\nmkdir /etc/vigil\nmkdir /etc/vigil/services\nmkdir /etc/vigil/services/getty\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	printf 'exec /bin/login\n' > /tmp/aegis-vigil-run
	printf 'respawn\nmax_restarts=5\n' > /tmp/aegis-vigil-policy
	printf 'VFS_OPEN VFS_READ VFS_WRITE AUTH\n' > /tmp/aegis-vigil-caps
	printf 'root\n' > /tmp/aegis-vigil-user
	printf 'write /tmp/aegis-vigil-run /etc/vigil/services/getty/run\nwrite /tmp/aegis-vigil-policy /etc/vigil/services/getty/policy\nwrite /tmp/aegis-vigil-caps /etc/vigil/services/getty/caps\nwrite /tmp/aegis-vigil-user /etc/vigil/services/getty/user\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	rm -f /tmp/aegis-vigil-run /tmp/aegis-vigil-policy /tmp/aegis-vigil-caps /tmp/aegis-vigil-user
	# httpd vigil service — binds :80, serves HTTP
	printf 'mkdir /etc/vigil/services/httpd\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	printf 'exec /bin/httpd\n' > /tmp/aegis-httpd-run
	printf 'respawn\nmax_restarts=5\n' > /tmp/aegis-httpd-policy
	printf 'NET_SOCKET VFS_OPEN VFS_READ\n' > /tmp/aegis-httpd-caps
	printf 'root\n' > /tmp/aegis-httpd-user
	printf 'write /tmp/aegis-httpd-run /etc/vigil/services/httpd/run\nwrite /tmp/aegis-httpd-policy /etc/vigil/services/httpd/policy\nwrite /tmp/aegis-httpd-caps /etc/vigil/services/httpd/caps\nwrite /tmp/aegis-httpd-user /etc/vigil/services/httpd/user\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	rm -f /tmp/aegis-httpd-run /tmp/aegis-httpd-policy /tmp/aegis-httpd-caps /tmp/aegis-httpd-user
	# dhcp vigil service — DHCP client with NET_ADMIN cap
	printf 'mkdir /etc/vigil/services/dhcp\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	printf 'exec /bin/dhcp\n' > /tmp/aegis-dhcp-run
	printf 'respawn\nmax_restarts=10\n' > /tmp/aegis-dhcp-policy
	printf 'NET_ADMIN NET_SOCKET\n' > /tmp/aegis-dhcp-caps
	printf 'root\n' > /tmp/aegis-dhcp-user
	printf 'write /tmp/aegis-dhcp-run /etc/vigil/services/dhcp/run\nwrite /tmp/aegis-dhcp-policy /etc/vigil/services/dhcp/policy\nwrite /tmp/aegis-dhcp-caps /etc/vigil/services/dhcp/caps\nwrite /tmp/aegis-dhcp-user /etc/vigil/services/dhcp/user\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	rm -f /tmp/aegis-dhcp-run /tmp/aegis-dhcp-policy /tmp/aegis-dhcp-caps /tmp/aegis-dhcp-user
	# curl binary and CA bundle on ext2
	printf 'mkdir /etc/ssl\nmkdir /etc/ssl/certs\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	printf 'write build/curl/curl /bin/curl\nwrite tools/cacert.pem /etc/ssl/certs/ca-certificates.crt\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
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
	$(MAKE) -C user/login clean 2>/dev/null; true
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
