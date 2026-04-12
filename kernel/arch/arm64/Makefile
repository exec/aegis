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
          -Wno-error=array-bounds \
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
            $(BUILD)/irq.o \
            $(BUILD)/stubs.o \
            $(BUILD)/ctx_switch.o \
            $(BUILD)/proc_enter.o \
            $(BUILD)/init_arm64_bin.o \
            $(BUILD)/shell_arm64_bin.o \
            $(BUILD)/ls_arm64_bin.o \
            $(BUILD)/cat_arm64_bin.o \
            $(BUILD)/echo_arm64_bin.o \
            $(BUILD)/pwd_arm64_bin.o \
            $(BUILD)/uname_arm64_bin.o \
            $(BUILD)/clear_arm64_bin.o \
            $(BUILD)/true_arm64_bin.o \
            $(BUILD)/false_arm64_bin.o \
            $(BUILD)/wc_arm64_bin.o \
            $(BUILD)/grep_arm64_bin.o \
            $(BUILD)/sort_arm64_bin.o \
            $(BUILD)/mkdir_arm64_bin.o \
            $(BUILD)/touch_arm64_bin.o \
            $(BUILD)/rm_arm64_bin.o \
            $(BUILD)/cp_arm64_bin.o \
            $(BUILD)/mv_arm64_bin.o \
            $(BUILD)/whoami_arm64_bin.o \
            $(BUILD)/login_arm64_bin.o

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

$(BUILD)/vectors.o: vectors.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/gic.o: gic.c | $(BUILD)
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

$(BUILD)/init_arm64_bin.o: init_arm64_bin.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ARM64 user binary blobs (generated by tools/build-arm64-userland.sh)
$(BUILD)/%_arm64_bin.o: %_arm64_bin.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

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

$(ELF): $(OBJS) linker.ld
	$(LD) -T linker.ld -nostdlib -o $@ $(OBJS) $(LIBGCC)

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
	@grep -q '\[VMM\] OK' /tmp/aegis-arm64-serial.txt && \
	  echo "PASS: [VMM] OK found" || \
	  (echo "FAIL: [VMM] OK not found"; cat /tmp/aegis-arm64-serial.txt; exit 1)

clean:
	rm -rf $(BUILD)
