# Makefile — ARM64 kernel build
#
# Usage:
#   make          — build build/aegis-arm64.elf
#   make run      — build + boot on QEMU virt with serial on stdio
#   make test     — build + boot headless, check for [PMM] OK
#   make clean    — remove build artifacts

CC      = aarch64-elf-gcc
LD      = aarch64-elf-ld
OBJCOPY = aarch64-elf-objcopy

# Root of the Aegis repo (two levels up from kernel/arch/arm64/)
REPO    = ../../..

# ── Userland blobs (embedded via objcopy, same pattern as x86) ────────
# Phase A4 of ARM64.md. The user blobs are pre-built into
# $(REPO)/build/blobs-arm64/*.bin by `tools/build-arm64-userland.sh`
# (which drives an aarch64-musl sysroot built from musl-1.2.5 and the
# aarch64-elf-gcc bare-metal cross compiler). This Makefile then uses
# objcopy -I binary -O elf64-littleaarch64 -B aarch64 to turn each .bin
# into a .o that exports _binary_<name>_bin_start/_end/_size — exactly
# the same symbols the x86 top-level Makefile produces for its blobs.
# That means kernel/fs/initrd.c and kernel/proc/proc.c can reference
# these symbols without any #ifdef dispatch; the initrd table and
# proc_spawn_init are arch-agnostic.
#
# Escape hatch: set ARM64_SKIP_USERLAND=1 to build the kernel with an
# empty blob (1 byte of zero) for every name. That keeps the symbols
# defined so linking succeeds, but initrd_open returns ENOENT for each
# /bin/* path, matching the old "userland not yet built" behaviour.
# Use this only for kernel-only bisects / CI runs that don't need
# userland — the default is to embed real blobs.
ARM64_SKIP_USERLAND ?= 0

BLOB_NAMES   = login vigil shell echo cat ls init
BLOB_SRC_DIR = $(REPO)/build/blobs-arm64
BLOB_OBJS    = $(patsubst %,$(BUILD)/blobs/%.o,$(BLOB_NAMES))

# Rust capability library (built from kernel/cap/ for aarch64-unknown-none)
CAP_LIB = $(REPO)/kernel/cap/target/aarch64-unknown-none/release/libcap.a
CARGO   = cargo +nightly

CFLAGS  = -ffreestanding -nostdlib -nostdinc \
          -isystem $(shell $(CC) -print-file-name=include) \
          -Wall -Wextra -Werror -O2 -g \
          -Wno-error=array-bounds \
          -march=armv8-a \
          -mgeneral-regs-only \
          -mno-outline-atomics \
          -I. \
          -I$(REPO)/kernel/core \
          -I$(REPO)/kernel/mm \
          -I$(REPO)/kernel/sched \
          -I$(REPO)/kernel/proc \
          -I$(REPO)/kernel/syscall \
          -I$(REPO)/kernel/fs \
          -I$(REPO)/kernel/tty \
          -I$(REPO)/kernel/signal \
          -I$(REPO)/kernel/drivers \
          -I$(REPO)/kernel/net \
          -I$(REPO)/kernel/cap

ASFLAGS = -g

BUILD   = build
ELF     = $(BUILD)/aegis-arm64.elf

# ARM64 arch-specific sources
ARCH_OBJS = $(BUILD)/boot.o \
            $(BUILD)/vectors.o \
            $(BUILD)/uart_pl011.o \
            $(BUILD)/main.o \
            $(BUILD)/arch_mm.o \
            $(BUILD)/arch_vmm.o \
            $(BUILD)/mmu_early.o \
            $(BUILD)/gic.o \
            $(BUILD)/gic_v3.o \
            $(BUILD)/irq.o \
            $(BUILD)/stubs.o \
            $(BUILD)/ctx_switch.o \
            $(BUILD)/proc_enter.o

# Shared kernel sources (arch-agnostic)
SHARED_OBJS = $(BUILD)/printk.o \
              $(BUILD)/random.o \
              $(BUILD)/cap_policy.o \
              $(BUILD)/pmm.o \
              $(BUILD)/vmm_arm64.o \
              $(BUILD)/vma.o \
              $(BUILD)/kva.o \
              $(BUILD)/sched.o \
              $(BUILD)/waitq.o \
              $(BUILD)/signal.o \
              $(BUILD)/tty.o \
              $(BUILD)/pty.o \
              $(BUILD)/syscall.o \
              $(BUILD)/sys_io.o \
              $(BUILD)/sys_memory.o \
              $(BUILD)/sys_process.o \
              $(BUILD)/sys_exec.o \
              $(BUILD)/sys_identity.o \
              $(BUILD)/sys_cap.o \
              $(BUILD)/sys_time.o \
              $(BUILD)/sys_file.o \
              $(BUILD)/sys_dir.o \
              $(BUILD)/sys_meta.o \
              $(BUILD)/sys_signal.o \
              $(BUILD)/sys_socket.o \
              $(BUILD)/sys_random.o \
              $(BUILD)/sys_disk.o \
              $(BUILD)/futex.o \
              $(BUILD)/fd_waitq.o \
              $(BUILD)/proc.o \
              $(BUILD)/elf.o \
              $(BUILD)/fd_table.o \
              $(BUILD)/vfs.o \
              $(BUILD)/initrd.o \
              $(BUILD)/console.o \
              $(BUILD)/kbd_vfs.o \
              $(BUILD)/pipe.o \
              $(BUILD)/blkdev.o \
              $(BUILD)/gpt.o \
              $(BUILD)/ext2.o \
              $(BUILD)/ext2_cache.o \
              $(BUILD)/ext2_dir.o \
              $(BUILD)/ext2_vfs.o \
              $(BUILD)/ramfs.o \
              $(BUILD)/procfs.o \
              $(BUILD)/memfd.o \
              $(BUILD)/poll_test.o \
              $(BUILD)/netdev.o \
              $(BUILD)/eth.o \
              $(BUILD)/ip.o \
              $(BUILD)/udp.o \
              $(BUILD)/tcp.o \
              $(BUILD)/socket.o \
              $(BUILD)/unix_socket.o \
              $(BUILD)/epoll.o \
              $(BUILD)/nvme.o \
              $(BUILD)/xhci.o \
              $(BUILD)/usb_hid.o \
              $(BUILD)/usb_mouse.o \
              $(BUILD)/virtio_net.o \
              $(BUILD)/rtl8169.o \
              $(BUILD)/ramdisk.o \
              $(BUILD)/fb.o

OBJS = $(ARCH_OBJS) $(SHARED_OBJS) $(BLOB_OBJS)

.PHONY: all run test clean blobs

# Keep generated .bin files around across builds. Without .PRECIOUS
# (or .SECONDARY) Make treats them as intermediates and deletes them
# after a successful build, which would force a ~2 minute rebuild of
# the musl sysroot on every incremental build.
.PRECIOUS: $(BLOB_SRC_DIR)/%.bin $(BUILD)/blobs/%.bin

all: $(ELF)

blobs: $(BLOB_OBJS)

# ── Blob recipes ──────────────────────────────────────────────────────
# objcopy each pre-built .bin into an ELF object. --rename-section
# mirrors the x86 recipe (turn .data into .rodata, readonly + alloc).
# The input format is "binary" (raw bytes); the output format is a
# standard little-endian aarch64 ELF, which ld slots in next to the
# real kernel objects.
#
# CRITICAL: objcopy -I binary derives the symbol names from the input
# FILE PATH, substituting non-alphanumeric characters with underscores.
# So `objcopy -I binary ../../../build/blobs-arm64/vigil.bin …` would
# produce `_binary_________build_blobs_arm64_vigil_bin_start` — NOT the
# `_binary_vigil_bin_start` the kernel expects. Match the x86 pipeline
# by copying each blob into $(BUILD)/blobs/<name>.bin first, then
# `cd`ing into that directory before running objcopy so the path is
# just `<name>.bin` and the symbols come out clean.
$(BUILD)/blobs/%.o: $(BLOB_SRC_DIR)/%.bin | $(BUILD)/blobs
	@cp $< $(BUILD)/blobs/$*.bin
	@cd $(BUILD)/blobs && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  $*.bin $*.o

$(BUILD)/blobs: | $(BUILD)
	mkdir -p $@

# Source rule: drive tools/build-arm64-userland.sh to produce the blob.
# This runs in a sub-shell; if the script aborts the Makefile fails.
# When ARM64_SKIP_USERLAND=1 the rule instead writes a zero-byte blob
# so objcopy still produces a well-formed .o. Even an empty blob is
# enough — the _binary_<name>_bin_start/_end symbols are defined with
# start == end, and initrd_open returns ENOENT for that path.
ifeq ($(ARM64_SKIP_USERLAND),1)
$(BLOB_SRC_DIR)/%.bin:
	@mkdir -p $(BLOB_SRC_DIR)
	@echo "[arm64] ARM64_SKIP_USERLAND=1: emitting empty $@ stub"
	@printf '\0' > $@
else
$(BLOB_SRC_DIR)/%.bin:
	@mkdir -p $(BLOB_SRC_DIR)
	bash $(REPO)/tools/build-arm64-userland.sh $*
endif


# ARM64 arch sources
$(BUILD)/boot.o: boot.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/uart_pl011.o: uart_pl011.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch_mm.o: arch_mm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vectors.o: vectors.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/gic.o: gic.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gic_v3.o: gic_v3.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/irq.o: irq.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/arch_vmm.o: arch_vmm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mmu_early.o: mmu_early.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/stubs.o: stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ctx_switch.o: ctx_switch.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/proc_enter.o: proc_enter.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# Shared kernel sources
$(BUILD)/printk.o: $(REPO)/kernel/core/printk.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: $(REPO)/kernel/mm/pmm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vma.o: $(REPO)/kernel/mm/vma.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tty.o: $(REPO)/kernel/tty/tty.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fd_table.o: $(REPO)/kernel/fs/fd_table.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vmm_arm64.o: vmm_arm64.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kva.o: $(REPO)/kernel/mm/kva.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sched.o: $(REPO)/kernel/sched/sched.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/syscall.o: $(REPO)/kernel/syscall/syscall.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_io.o: $(REPO)/kernel/syscall/sys_io.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_memory.o: $(REPO)/kernel/syscall/sys_memory.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_process.o: $(REPO)/kernel/syscall/sys_process.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_file.o: $(REPO)/kernel/syscall/sys_file.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_signal.o: $(REPO)/kernel/syscall/sys_signal.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_socket.o: $(REPO)/kernel/syscall/sys_socket.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fd_waitq.o: $(REPO)/kernel/syscall/fd_waitq.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vfs.o: $(REPO)/kernel/fs/vfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/initrd.o: $(REPO)/kernel/fs/initrd.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/console.o: $(REPO)/kernel/fs/console.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kbd_vfs.o: $(REPO)/kernel/fs/kbd_vfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pipe.o: $(REPO)/kernel/fs/pipe.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/blkdev.o: $(REPO)/kernel/fs/blkdev.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gpt.o: $(REPO)/kernel/fs/gpt.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ext2.o: $(REPO)/kernel/fs/ext2.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ext2_cache.o: $(REPO)/kernel/fs/ext2_cache.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ext2_dir.o: $(REPO)/kernel/fs/ext2_dir.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ramfs.o: $(REPO)/kernel/fs/ramfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/signal.o: $(REPO)/kernel/signal/signal.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/proc.o: $(REPO)/kernel/proc/proc.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf.o: $(REPO)/kernel/proc/elf.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/netdev.o: $(REPO)/kernel/net/netdev.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/eth.o: $(REPO)/kernel/net/eth.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ip.o: $(REPO)/kernel/net/ip.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/udp.o: $(REPO)/kernel/net/udp.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tcp.o: $(REPO)/kernel/net/tcp.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/socket.o: $(REPO)/kernel/net/socket.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/epoll.o: $(REPO)/kernel/net/epoll.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/nvme.o: $(REPO)/kernel/drivers/nvme.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/xhci.o: $(REPO)/kernel/drivers/xhci.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/usb_hid.o: $(REPO)/kernel/drivers/usb_hid.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/virtio_net.o: $(REPO)/kernel/drivers/virtio_net.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fb.o: $(REPO)/kernel/drivers/fb.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/random.o: $(REPO)/kernel/core/random.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sys_random.o: $(REPO)/kernel/syscall/sys_random.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Additional shared sources
$(BUILD)/cap_policy.o: $(REPO)/kernel/cap/cap_policy.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/waitq.o: $(REPO)/kernel/sched/waitq.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/pty.o: $(REPO)/kernel/tty/pty.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sys_exec.o: $(REPO)/kernel/syscall/sys_exec.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sys_identity.o: $(REPO)/kernel/syscall/sys_identity.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sys_cap.o: $(REPO)/kernel/syscall/sys_cap.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sys_time.o: $(REPO)/kernel/syscall/sys_time.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sys_dir.o: $(REPO)/kernel/syscall/sys_dir.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sys_meta.o: $(REPO)/kernel/syscall/sys_meta.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sys_disk.o: $(REPO)/kernel/syscall/sys_disk.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/futex.o: $(REPO)/kernel/syscall/futex.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/ext2_vfs.o: $(REPO)/kernel/fs/ext2_vfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/procfs.o: $(REPO)/kernel/fs/procfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/memfd.o: $(REPO)/kernel/fs/memfd.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/poll_test.o: $(REPO)/kernel/fs/poll_test.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/unix_socket.o: $(REPO)/kernel/net/unix_socket.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/usb_mouse.o: $(REPO)/kernel/drivers/usb_mouse.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/rtl8169.o: $(REPO)/kernel/drivers/rtl8169.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/ramdisk.o: $(REPO)/kernel/drivers/ramdisk.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

LIBGCC := $(shell $(CC) -print-libgcc-file-name)

# Rust capability library — built via cargo for aarch64-unknown-none.
# Order in the link line: $(CAP_LIB) before $(LIBGCC) so libgcc's atomic
# helpers satisfy any remaining references emitted by the Rust codegen.
$(CAP_LIB): $(REPO)/kernel/cap/src/lib.rs $(REPO)/kernel/cap/Cargo.toml
	$(CARGO) build --release \
	    --target aarch64-unknown-none \
	    --manifest-path $(REPO)/kernel/cap/Cargo.toml

$(ELF): $(OBJS) $(CAP_LIB) linker.ld
	$(LD) -T linker.ld -nostdlib -o $@ $(OBJS) $(CAP_LIB) $(LIBGCC)

$(BUILD):
	mkdir -p $(BUILD)

run: $(ELF)
	qemu-system-aarch64 \
	  -machine virt -cpu cortex-a72 \
	  -m 128M -nographic \
	  -kernel $(ELF)

# ── Linux arm64 Image format ──────────────────────────────────────────
# Produces build/aegis.img — a flat binary with the 64-byte arm64 boot
# header, suitable for both QEMU virt's `-kernel` Image loader and
# Raspberry Pi firmware (start4.elf / start5.elf). The header lives
# inline in boot.S's .text.boot section (see that file for details),
# so gen-arm64-image.py only has to flatten the ELF with objcopy and
# stamp the image_size field. See README.arm64.md for the Pi flow.
IMG = $(BUILD)/aegis.img

.PHONY: image run-image

image: $(IMG)

$(IMG): $(ELF) $(REPO)/tools/gen-arm64-image.py
	python3 $(REPO)/tools/gen-arm64-image.py $(ELF) $(IMG)

# Convenience: boot the Image format directly (the path the Pi will use).
run-image: $(IMG)
	qemu-system-aarch64 \
	  -machine virt -cpu cortex-a72 \
	  -m 2G -nographic \
	  -kernel $(IMG)

test: $(ELF)
	@echo "--- arm64 boot test ---"
	@gtimeout 10s qemu-system-aarch64 \
	  -machine virt -cpu cortex-a72 \
	  -m 128M -nographic \
	  -kernel $(ELF) 2>/dev/null \
	  | tee /tmp/aegis-arm64-serial.txt || true
	@grep -q '\[VMM\] OK' /tmp/aegis-arm64-serial.txt && \
	  echo "PASS: [VMM] OK found" || \
	  (echo "FAIL: [VMM] OK not found"; cat /tmp/aegis-arm64-serial.txt; exit 1)

clean:
	rm -rf $(BUILD)
