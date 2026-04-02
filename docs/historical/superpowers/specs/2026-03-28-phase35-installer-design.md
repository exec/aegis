# Phase 35: Text-Mode Installer — Design Spec

## Goal

A text-mode installer that partitions an NVMe disk with Aegis GPT GUIDs, copies the live root filesystem to disk, installs GRUB, and produces a bootable installed system. After installation, the system boots from NVMe without the ISO.

## Problem

The live system boots from ISO (rootfs.img in RAM via multiboot2 module). All writes are volatile. There is no way to persist the system to disk. The kernel has no userspace block device access — `blkdev_t` is kernel-internal.

## Architecture

**New syscalls expose raw block device I/O to userspace**, gated by a new `CAP_KIND_DISK_ADMIN` capability. The installer is a userspace binary (`/bin/installer`) that uses these syscalls to:

1. Write a GPT partition table to the NVMe disk
2. Copy the rootfs ext2 image from ramdisk to the Aegis root partition
3. Write GRUB boot stages to the BIOS Boot Partition
4. Write a grub.cfg that targets the specific installed partition

The installer runs interactively from the live system shell. It is NOT automatic — the user explicitly invokes it.

---

## Component 1: Block Device Syscalls

Two new syscalls for raw block I/O, gated by `CAP_KIND_DISK_ADMIN`:

### sys_blkdev_list (syscall 510)

```c
/* Enumerate registered block devices.
 * buf: user buffer to receive device info structs
 * bufsize: byte size of buffer
 * Returns number of devices, or negative errno. */
int64_t sys_blkdev_list(uint64_t buf, uint64_t bufsize);
```

Each entry in the buffer:
```c
typedef struct {
    char     name[16];       /* e.g. "nvme0", "ramdisk0" */
    uint64_t block_count;    /* total blocks */
    uint32_t block_size;     /* bytes per block (512) */
    uint32_t _pad;
} blkdev_info_t;
```

### sys_blkdev_io (syscall 511)

```c
/* Read or write raw blocks on a named block device.
 * name: user pointer to NUL-terminated device name
 * lba: starting logical block address
 * count: number of blocks
 * buf: user buffer
 * write: 0=read, 1=write
 * Returns 0 on success, negative errno on failure. */
int64_t sys_blkdev_io(uint64_t name, uint64_t lba, uint64_t count,
                       uint64_t buf, uint64_t write);
```

Both syscalls check `CAP_KIND_DISK_ADMIN`. The installer binary receives this capability via vigil's exec_caps mechanism.

The `sys_blkdev_io` implementation:
1. `copy_from_user` the device name string
2. `blkdev_get(name)` to find the device
3. For reads: `dev->read(dev, lba, count, kbuf)` → `copy_to_user(user_buf, kbuf, count*512)`
4. For writes: `copy_from_user(kbuf, user_buf, count*512)` → `dev->write(dev, lba, count, kbuf)`
5. Use a static 4KB kernel bounce buffer, looping in 8-sector chunks (NVMe max transfer)

---

## Component 2: CAP_KIND_DISK_ADMIN

Add a new capability kind to the Rust cap subsystem:

```rust
pub const CAP_KIND_DISK_ADMIN: u32 = 11;
```

Update `cap_init` to include the new kind in the total count (10 → 11).

The installer vigil service config grants `DISK_ADMIN`:
```
# /etc/vigil/services/installer/caps
DISK_ADMIN VFS_OPEN VFS_READ VFS_WRITE
```

The execve baseline does NOT include DISK_ADMIN — only the installer binary gets it.

---

## Component 3: GPT Creation (Userspace)

The installer binary writes a GPT partition table to the NVMe disk. This happens entirely in userspace using `sys_blkdev_io`:

### Partition layout

| # | Name | Type GUID | Size | Purpose |
|---|------|-----------|------|---------|
| 1 | bios-boot | `21686148-6449-6E6F-744E-656564454649` | 1 MB | GRUB core.img (BIOS boot) |
| 2 | aegis-root | `A3618F24-0C76-4B3D-0001-000000000000` | Remaining - 1MB | ext2 root filesystem |

The BIOS Boot Partition GUID (`21686148-...`) is the standard GUID defined by GRUB for GPT+BIOS booting. GRUB recognizes this automatically.

No swap partition in the initial install — swap is future work.

### GPT writing logic

The installer binary must:
1. Zero the first 34 sectors (protective MBR + primary GPT header + entries)
2. Write a protective MBR at LBA 0
3. Write the primary GPT header at LBA 1
4. Write partition entries at LBAs 2-33
5. Write backup partition entries and header at the end of disk
6. Compute CRC32 checksums for header and entries

This is ~200 lines of C. The CRC32 implementation is straightforward (same polynomial as gpt.c's kernel-side CRC32).

---

## Component 4: Rootfs Copy (Userspace)

The installer copies the ramdisk's ext2 data to the root partition on NVMe.

The ramdisk blkdev is `ramdisk0`. The NVMe root partition (after GPT is written and kernel re-scans) is `nvme0p2` (partition 2, since partition 1 is the BIOS boot partition).

The copy loop:
```c
for (lba = 0; lba < rootfs_blocks; lba += 8) {
    count = min(8, rootfs_blocks - lba);
    sys_blkdev_io("ramdisk0", lba, count, buf, 0);  /* read from ramdisk */
    sys_blkdev_io("nvme0p2", lba, count, buf, 1);   /* write to NVMe partition */
}
```

After the copy, the NVMe root partition has an identical ext2 filesystem.

### Kernel re-scan

After the installer writes the GPT, the kernel needs to re-scan partitions. Add `sys_gpt_rescan` (syscall 512):

```c
/* Re-scan GPT on the named block device. Unregisters old partitions,
 * reads new GPT, registers new partitions.
 * Requires CAP_KIND_DISK_ADMIN.
 * Returns number of partitions found, or negative errno. */
int64_t sys_gpt_rescan(uint64_t name);
```

This calls `gpt_scan(name)` in the kernel after clearing old partition blkdevs.

---

## Component 5: GRUB Installation

The installed system needs GRUB to boot from the NVMe disk. GRUB on BIOS+GPT requires:

1. **Protective MBR** with GRUB stage 1 (boot.img, 440 bytes) — the MBR bootstrap code that loads core.img from the BIOS Boot Partition
2. **BIOS Boot Partition** (1MB) containing GRUB core.img — the main GRUB binary that understands ext2 and loads the kernel
3. **grub.cfg** on the ext2 root partition at `/boot/grub/grub.cfg`

### Pre-built GRUB images

The Makefile builds GRUB boot images at build time using `grub-mkimage`:

```bash
# Build GRUB core.img for BIOS+GPT boot from ext2
grub-mkimage -O i386-pc -o build/grub-core.img \
    -p '(hd0,gpt2)/boot/grub' \
    biosdisk part_gpt ext2 normal multiboot2 boot
```

The `-p` flag sets the prefix: GRUB looks for its config on partition 2 of the first disk at `/boot/grub`. This matches our partition layout (partition 2 = aegis-root).

`boot.img` is copied from `/usr/lib/grub/i386-pc/boot.img`.

Both `build/grub-core.img` and the `boot.img` are embedded in the rootfs.img at `/boot/grub/core.img` and `/boot/grub/boot.img`.

### Installer GRUB write

1. Write `boot.img` (440 bytes) to LBA 0, bytes 0-439 (MBR bootstrap code). Preserve the rest of the MBR (partition table at offset 446, signature at 510-511).
2. Write `core.img` to the BIOS Boot Partition (starting at its first LBA). The core.img is typically ~30-60KB.
3. Write `grub.cfg` to the ext2 root partition at `/boot/grub/grub.cfg`.

### Installed grub.cfg

The installed grub.cfg targets the specific NVMe partition:

```
set timeout=3
set default=0
insmod all_video
insmod gfxterm
set gfxmode=1024x768x32,auto
terminal_input console
terminal_output gfxterm

menuentry "Aegis" {
    set gfxpayload=keep
    set root=(hd0,gpt2)
    multiboot2 /boot/aegis.elf
    boot
}
```

Note: NO `module2` line. The installed system boots the kernel from ext2 directly. No ramdisk needed — ext2 is on NVMe. The kernel detects NVMe → `gpt_scan` finds the Aegis root partition → `ext2_mount("nvme0p1")` succeeds.

Wait — after GPT re-scan with the new layout, partition numbering changes:
- Partition 1 = BIOS Boot (not Aegis GUID → not registered by gpt_scan)
- Partition 2 = Aegis Root (Aegis GUID → registered as `nvme0p1`)

So `nvme0p1` maps to GPT partition 2 (the Aegis root). This is because `gpt_scan` only registers Aegis-prefixed partitions and numbers them sequentially. `set root=(hd0,gpt2)` is the GRUB perspective; the kernel sees it as `nvme0p1`.

The installed grub.cfg must also embed the kernel in rootfs.img. So the Makefile must copy `aegis.elf` into rootfs.img at `/boot/aegis.elf`. Currently it's only in the ISO.

---

## Component 6: Kernel Binary in rootfs.img

Add `aegis.elf` to the rootfs.img at `/boot/aegis.elf`. The Makefile already builds the ELF and the rootfs — just add a debugfs write:

```makefile
printf 'mkdir /boot\nmkdir /boot/grub\nwrite $(BUILD)/aegis.elf /boot/aegis.elf\n' \
    | /sbin/debugfs -w $(ROOTFS)
```

Also write the pre-built GRUB images:
```makefile
printf 'write $(BUILD)/grub-boot.img /boot/grub/boot.img\nwrite $(BUILD)/grub-core.img /boot/grub/core.img\n' \
    | /sbin/debugfs -w $(ROOTFS)
```

---

## Component 7: Installer Binary

A new userspace program at `user/installer/main.c`. Text-mode interactive installer:

### Flow

```
=== Aegis Installer ===

This will install Aegis to your NVMe disk.
WARNING: All data on the disk will be destroyed!

Available disks:
  nvme0: 131072 sectors (64 MB)

Install to nvme0? [y/N] y

Creating GPT partition table... done
  Partition 1: bios-boot (1 MB)
  Partition 2: aegis-root (62 MB)

Rescanning partitions... done
  Found: nvme0p1 (aegis-root, 62 MB)

Copying root filesystem... done (59 MB copied)
Installing GRUB bootloader... done
Syncing disk... done

Installation complete!
Remove the ISO and reboot to start Aegis from disk.
```

### Implementation

The installer:
1. Calls `sys_blkdev_list` to enumerate disks (shows NVMe, skips ramdisk)
2. Prompts for confirmation
3. Writes GPT using `sys_blkdev_io` (raw block writes to `nvme0`)
4. Calls `sys_gpt_rescan("nvme0")` to register new partitions
5. Copies rootfs: reads from `ramdisk0`, writes to `nvme0p1` (the Aegis root partition)
6. Copies GRUB: reads boot.img and core.img from ext2 files, writes via `sys_blkdev_io`
7. Writes grub.cfg to ext2 root partition via regular file I/O (`open`/`write`)
8. Calls `sync()` to flush ext2 cache

### GRUB installation detail

The installer reads `/boot/grub/boot.img` and `/boot/grub/core.img` from the live filesystem (ramdisk ext2). It writes:
- `boot.img` bytes 0-439 → LBA 0 of `nvme0` (MBR bootstrap area)
- `core.img` → BIOS Boot Partition on `nvme0` (starts at partition 1's first LBA)

### Vigil service

The installer is NOT a vigil service. It's a standalone binary invoked manually from the shell: `installer` or `/bin/installer`.

To get `CAP_KIND_DISK_ADMIN`, the installer needs exec_caps. Since it's launched from the shell (not vigil), it gets baseline execve caps. Add `CAP_KIND_DISK_ADMIN` to the execve baseline? No — that's too permissive. Every binary would get disk admin.

**Better approach:** Add a `DISK_ADMIN` entry to the execve baseline only when the installer binary is detected (by path). Or simpler: add a new syscall `sys_cap_request(kind)` that interactively prompts the user for authorization. Or simplest: just add DISK_ADMIN to the execve baseline for now (it's a single-user system with no untrusted code), with a TODO to restrict it later.

**Decision:** Add DISK_ADMIN to the execve baseline. It's gated by a capability check so random programs that don't call the disk syscalls are unaffected. The security audit (Phase 38) can restrict it.

---

## Component 8: Installed Boot Path

After installation, the system boots from NVMe:

1. BIOS loads MBR (boot.img) from NVMe LBA 0
2. boot.img loads core.img from the BIOS Boot Partition
3. core.img initializes GRUB, reads grub.cfg from `(hd0,gpt2)/boot/grub/grub.cfg`
4. GRUB loads `(hd0,gpt2)/boot/aegis.elf` via multiboot2
5. Kernel starts — no module (no `module2` in installed grub.cfg)
6. `ramdisk_init` gets phys=0, skips (no module)
7. NVMe init → `gpt_scan` finds Aegis root → `ext2_mount("nvme0p1")` succeeds
8. System boots normally from persistent NVMe ext2

---

## Testing

### test_installer.py

Boots the live ISO with an empty NVMe disk (q35). Logs in, runs `installer`, answers `y` to confirmation. Reboots from the NVMe disk (remove ISO from QEMU command). Verifies the system boots and login works.

Two QEMU boots:
1. Boot 1: ISO + empty NVMe → run installer → shutdown
2. Boot 2: NVMe only (no ISO) → verify boot + login

The installed boot should NOT have `[RAMDISK]` in serial output (no module). It should have `[EXT2] OK: mounted nvme0p1`.

### Manual test

```bash
# Create empty 128MB disk
dd if=/dev/zero of=/tmp/test-disk.img bs=1M count=128
# Boot live ISO with empty NVMe
qemu-system-x86_64 -machine q35 -cdrom build/aegis.iso \
    -drive file=/tmp/test-disk.img,if=none,id=nvme0 \
    -device nvme,drive=nvme0,serial=aegis0 \
    -serial stdio -m 2G
# In shell: run installer, answer y
# Shutdown, then boot from NVMe only:
qemu-system-x86_64 -machine q35 \
    -drive file=/tmp/test-disk.img,if=none,id=nvme0 \
    -device nvme,drive=nvme0,serial=aegis0 \
    -serial stdio -m 2G
```

---

## Forward Constraints

1. **No UEFI boot.** The installer creates BIOS+GPT boot only. UEFI boot requires an EFI System Partition and a different GRUB image. Future work.

2. **Single NVMe only.** The installer targets `nvme0`. Multiple NVMe support (device selection) is future work.

3. **No resize/dual-boot.** The installer wipes the entire disk. No partition resizing, no dual-boot with other OSes.

4. **GRUB prefix is hardcoded.** `grub-mkimage -p '(hd0,gpt2)/boot/grub'` assumes the root partition is GPT partition 2 on the first disk. If the partition layout changes, the GRUB image must be rebuilt.

5. **Kernel binary in rootfs.** The installed system's kernel is at `/boot/aegis.elf` inside the ext2 root partition. Kernel updates require writing the new ELF to this path and rebooting.

6. **No installer on installed systems.** Running the installer on an already-installed system would destroy the existing installation. A warning should be shown if NVMe already has an Aegis root partition.

7. **DISK_ADMIN is in the execve baseline.** Every exec'd binary gets this capability. The security audit (Phase 38) should restrict it to specific binaries or require interactive authorization.
