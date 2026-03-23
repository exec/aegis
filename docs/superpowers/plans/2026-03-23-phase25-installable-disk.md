# Phase 25: Installable Disk Image Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a GPT-partitioned disk image with an EFI System Partition (GRUB EFI) and an ext2 root partition, enabling Aegis to boot from NVMe on real UEFI hardware and be installed via `dd` to a physical disk.

**Architecture:** The NVMe driver gains GPT partition parsing: after init, it reads LBA 1 (GPT header), walks the partition entry array, and registers each partition as a sub-device (`nvme0p1`, `nvme0p2`, etc.) via `blkdev_register()` with the appropriate LBA offset. The ext2 mount switches from `"nvme0"` (raw device) to `"nvme0p2"` (second partition). A new `tools/mkdisk.sh` script creates the full disk image using `sgdisk`, `mkfs.fat`, `mkfs.ext2`, `grub-install`, and `losetup`. New Makefile targets: `make disk` (creates image, requires sudo), `make run-disk` (boots via OVMF + NVMe), `make install` (writes to physical device).

**Tech Stack:** C (GPT parsing in kernel), bash (mkdisk.sh), GRUB EFI (`grub-install --target=x86_64-efi`), OVMF (UEFI firmware for QEMU), `sgdisk` (GPT partitioning), FAT32 ESP.

**Spec:** docs/superpowers/specs/2026-03-23-aegis-v1-design.md — Phase 25

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/drivers/nvme.c` | Modify | After NVMe init, read GPT header at LBA 1, register partition sub-devices |
| `kernel/fs/ext2.c` | Modify | Change mount from `"nvme0"` to `"nvme0p2"` |
| `tools/mkdisk.sh` | Create | GPT disk image creation (ESP + ext2 root), requires sudo |
| `tools/mk-ovmf-vars.sh` | Create | Copy system OVMF_VARS to build/ for writable UEFI variables |
| `Makefile` | Modify | Add `disk`, `run-disk`, `install` targets; update OVMF paths |
| `tests/test_disk.py` | Create | Boot aegis.img via NVMe, verify shell works |
| `tests/run_tests.sh` | Modify | Wire in test_disk.py |

---

### Task 1: Add GPT partition parsing to nvme.c

**Files:**
- Modify: `kernel/drivers/nvme.c`

- [ ] **Step 1: Define GPT structures**

Add to `nvme.c` (or a new `gpt.h` if preferred):

```c
/* GPT Header — LBA 1 (512 bytes) */
typedef struct __attribute__((packed)) {
    char     signature[8];      /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} gpt_header_t;

/* GPT Partition Entry — 128 bytes */
typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name[36];          /* UTF-16LE name */
} gpt_entry_t;
```

- [ ] **Step 2: Implement GPT scanning after NVMe init**

After the NVMe controller is initialized and `nvme0` is registered:

```c
static void nvme_scan_gpt(blkdev_t *whole_disk)
{
    uint8_t buf[512];

    /* Read LBA 1 — GPT header */
    if (whole_disk->read(whole_disk, 1, 1, buf) != 0)
        return;

    gpt_header_t *hdr = (gpt_header_t *)buf;
    if (memcmp(hdr->signature, "EFI PART", 8) != 0)
        return;

    /* Read partition entries (starting at partition_entry_lba) */
    uint32_t entries_per_sector = 512 / hdr->partition_entry_size;
    uint32_t sectors_needed = (hdr->num_partition_entries +
                                entries_per_sector - 1) / entries_per_sector;

    uint8_t entry_buf[512];
    int part_num = 0;
    uint32_t sector;

    for (sector = 0; sector < sectors_needed; sector++) {
        if (whole_disk->read(whole_disk,
                              hdr->partition_entry_lba + sector,
                              1, entry_buf) != 0)
            break;

        uint32_t j;
        for (j = 0; j < entries_per_sector && part_num < 8; j++) {
            gpt_entry_t *e = (gpt_entry_t *)(entry_buf +
                              j * hdr->partition_entry_size);

            /* Skip empty entries (type GUID all zeros) */
            int empty = 1;
            int k;
            for (k = 0; k < 16; k++) {
                if (e->type_guid[k] != 0) { empty = 0; break; }
            }
            if (empty) continue;

            part_num++;

            /* Allocate a per-partition device context that holds the LBA offset.
             * nvme_part_read/write add lba_offset before calling the whole-disk
             * callbacks, so reads from nvme0p2 go to the correct disk sectors. */
            nvme_part_dev_t *pd = (nvme_part_dev_t *)kva_alloc_pages(1);
            pd->disk       = whole_disk;
            pd->lba_offset = e->starting_lba;
            /* Format name: nvme0p1, nvme0p2, etc. */
            pd->base.name[0] = 'n'; pd->base.name[1] = 'v'; pd->base.name[2] = 'm';
            pd->base.name[3] = 'e'; pd->base.name[4] = '0'; pd->base.name[5] = 'p';
            pd->base.name[6] = '0' + part_num; pd->base.name[7] = '\0';
            pd->base.block_count = e->ending_lba - e->starting_lba + 1;
            pd->base.block_size  = 512;
            pd->base.read        = nvme_part_read;
            pd->base.write       = nvme_part_write;
            pd->base.priv        = NULL;  /* context is in nvme_part_dev_t itself */

            blkdev_register(&pd->base);
            printk("[NVME] partition %s: LBA %lu-%lu\n",
                   pd->base.name, e->starting_lba, e->ending_lba);
        }
    }
}
```

The LBA-offset wrapper functions and per-partition context type must be defined before `nvme_scan_gpt`:

```c
/* Per-partition context — embeds blkdev_t so it can be cast to blkdev_t * */
typedef struct {
    blkdev_t     base;
    blkdev_t    *disk;
    uint64_t     lba_offset;
} nvme_part_dev_t;

static int nvme_part_read(struct blkdev *dev, uint64_t lba,
                           uint32_t count, void *buf)
{
    nvme_part_dev_t *p = (nvme_part_dev_t *)dev;
    return p->disk->read(p->disk, lba + p->lba_offset, count, buf);
}

static int nvme_part_write(struct blkdev *dev, uint64_t lba,
                            uint32_t count, const void *buf)
{
    nvme_part_dev_t *p = (nvme_part_dev_t *)dev;
    return p->disk->write(p->disk, lba + p->lba_offset, count, buf);
}
```

This ensures that a read of LBA 0 from `nvme0p2` translates to a read of LBA `starting_lba` on the underlying NVMe disk, not LBA 0 of the whole disk.

- [ ] **Step 3: Call nvme_scan_gpt after blkdev registration**

```c
    /* In nvme_init(), after blkdev_register(&s_nvme_dev): */
    nvme_scan_gpt(&s_nvme_dev);
```

- [ ] **Step 4: Build**

```bash
make 2>&1 | tail -20
```

- [ ] **Step 5: Commit**

```bash
git add kernel/drivers/nvme.c
git commit -m "drivers: add GPT partition parsing to NVMe driver"
```

---

### Task 2: Create tools/mkdisk.sh and tools/mk-ovmf-vars.sh

**Files:**
- Create: `tools/mkdisk.sh`
- Create: `tools/mk-ovmf-vars.sh`

- [ ] **Step 1: Create tools/mkdisk.sh**

```bash
#!/bin/bash
# mkdisk.sh — Create a GPT disk image with ESP + ext2 root for Aegis
#
# Requires: sgdisk, losetup, mkfs.fat, mkfs.ext2, grub-install, mount
# Must run as root (uses losetup and mount).
#
# Usage: sudo ./tools/mkdisk.sh

set -euo pipefail

BUILD="${1:-build}"
IMG="${BUILD}/aegis.img"
SIZE_MB=256

echo "[mkdisk] Creating ${SIZE_MB}MB disk image at ${IMG}"
dd if=/dev/zero of="${IMG}" bs=1M count=${SIZE_MB} status=progress

echo "[mkdisk] Creating GPT partition table"
sgdisk -Z "${IMG}"
sgdisk -n 1:2048:+32M -t 1:ef00 -c 1:"EFI System" "${IMG}"
sgdisk -n 2:0:0       -t 2:8300 -c 2:"Aegis Root"  "${IMG}"
sgdisk -p "${IMG}"

echo "[mkdisk] Attaching loop device"
LOOP=$(losetup -fP --show "${IMG}")
echo "[mkdisk] Loop device: ${LOOP}"

cleanup() {
    echo "[mkdisk] Cleaning up"
    umount /mnt/aegis-esp  2>/dev/null || true
    umount /mnt/aegis-root 2>/dev/null || true
    losetup -d "${LOOP}"   2>/dev/null || true
}
trap cleanup EXIT

echo "[mkdisk] Formatting ESP (FAT32)"
mkfs.fat -F32 "${LOOP}p1"

echo "[mkdisk] Formatting root (ext2)"
mkfs.ext2 -F -L aegis-root "${LOOP}p2"

# Mount ESP
mkdir -p /mnt/aegis-esp
mount "${LOOP}p1" /mnt/aegis-esp

echo "[mkdisk] Installing GRUB EFI"
grub-install --target=x86_64-efi \
             --efi-directory=/mnt/aegis-esp \
             --boot-directory=/mnt/aegis-esp/boot \
             --removable \
             --no-nvram

# Get ESP UUID for grub.cfg
ESP_UUID=$(blkid -s UUID -o value "${LOOP}p1")

mkdir -p /mnt/aegis-esp/boot/grub
cat > /mnt/aegis-esp/boot/grub/grub.cfg << GRUBEOF
set timeout=3
set default=0

menuentry "Aegis" {
    insmod part_gpt
    insmod fat
    insmod multiboot2
    search --no-floppy --fs-uuid --set=root ${ESP_UUID}
    multiboot2 /boot/aegis.elf
    boot
}
GRUBEOF

echo "[mkdisk] Copying kernel to ESP"
cp "${BUILD}/aegis.elf" /mnt/aegis-esp/boot/aegis.elf

umount /mnt/aegis-esp

# Mount ext2 root
mkdir -p /mnt/aegis-root
mount "${LOOP}p2" /mnt/aegis-root

echo "[mkdisk] Populating ext2 root"
mkdir -p /mnt/aegis-root/{bin,etc,tmp,home}

# Copy userspace binaries
for bin in shell ls cat echo pwd uname clear true false wc grep sort; do
    if [ -f "${BUILD}/${bin}.elf" ]; then
        cp "${BUILD}/${bin}.elf" "/mnt/aegis-root/bin/${bin}"
    fi
done
# Shell is also available as /bin/sh
if [ -f "${BUILD}/shell.elf" ]; then
    cp "${BUILD}/shell.elf" /mnt/aegis-root/bin/sh
fi

echo "Welcome to Aegis" > /mnt/aegis-root/etc/motd

umount /mnt/aegis-root

echo "[mkdisk] Done: ${IMG} (${SIZE_MB}MB, GPT, ESP+ext2)"
```

- [ ] **Step 2: Create tools/mk-ovmf-vars.sh**

```bash
#!/bin/bash
# mk-ovmf-vars.sh — Copy system OVMF_VARS to build/ for writable UEFI variables
#
# QEMU UEFI boot requires a writable OVMF_VARS file (UEFI variable store).
# The system-provided file is read-only; we copy it to build/.

set -euo pipefail

BUILD="${1:-build}"
VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
VARS_DST="${BUILD}/OVMF_VARS_4M.fd"

if [ ! -f "${VARS_SRC}" ]; then
    echo "ERROR: ${VARS_SRC} not found. Install: sudo apt install ovmf"
    exit 1
fi

mkdir -p "${BUILD}"
cp "${VARS_SRC}" "${VARS_DST}"
echo "Copied OVMF_VARS to ${VARS_DST}"
```

- [ ] **Step 3: Make scripts executable**

```bash
chmod +x tools/mkdisk.sh tools/mk-ovmf-vars.sh
```

- [ ] **Step 4: Commit**

```bash
git add tools/mkdisk.sh tools/mk-ovmf-vars.sh
git commit -m "tools: add mkdisk.sh (GPT ESP+ext2 image) and mk-ovmf-vars.sh"
```

---

### Task 3: Add Makefile targets (disk, run-disk, install)

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add `disk` target**

```makefile
# Create installable GPT disk image (requires sudo)
disk: $(BUILD)/aegis.elf
	sudo ./tools/mkdisk.sh $(BUILD)
```

- [ ] **Step 2: Add `run-disk` target**

```makefile
# Boot disk image via UEFI (OVMF) + NVMe
OVMF_CODE = /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS = $(BUILD)/OVMF_VARS_4M.fd

run-disk: $(BUILD)/aegis.img $(OVMF_VARS)
	qemu-system-x86_64 \
	    -machine q35 -cpu Broadwell \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(OVMF_VARS) \
	    -drive file=$(BUILD)/aegis.img,if=none,id=nvme0 \
	    -device nvme,drive=nvme0,serial=aegis0 \
	    -device qemu-xhci -device usb-kbd \
	    -m 256M -serial stdio -display gtk

$(OVMF_VARS):
	./tools/mk-ovmf-vars.sh $(BUILD)
```

- [ ] **Step 3: Add `install` target**

```makefile
# Write disk image to physical device (DANGEROUS — requires confirmation)
install: $(BUILD)/aegis.img
	@echo "WARNING: this will ERASE the target disk."
	@read -p "Type the full device path (e.g. /dev/sdb) to confirm: " DISK; \
	    dd if=$(BUILD)/aegis.img of=$$DISK bs=4M status=progress
```

- [ ] **Step 4: Update ext2_mount call in main.c**

Change from `ext2_mount("nvme0")` to `ext2_mount("nvme0p2")` — the GPT-partitioned disk has the ext2 root on partition 2. But also keep a fallback: try `"nvme0p2"` first, fall back to `"nvme0"` for raw ext2 images:

```c
    if (ext2_mount("nvme0p2") != 0)
        ext2_mount("nvme0");    /* fallback: raw ext2 (no partition table) */
```

- [ ] **Step 5: Build and run make test**

```bash
make test 2>&1 | tail -10
```

`make test` should remain GREEN (no NVMe on `-machine pc`).

- [ ] **Step 6: Commit**

```bash
git add Makefile kernel/core/main.c
git commit -m "phase25: add disk/run-disk/install Makefile targets, GPT-aware ext2 mount"
```

---

### Task 4: Write test_disk.py and wire into run_tests.sh

**Files:**
- Create: `tests/test_disk.py`
- Modify: `tests/run_tests.sh`

- [ ] **Step 1: Create test_disk.py**

Test flow:
1. Create a 256MB GPT disk image with ESP + ext2 root (programmatically, without sudo — use `sgdisk` + `mke2fs` on the raw image, skip GRUB install for test purposes)
2. Alternative: use the simpler approach of creating a raw ext2 image (like test_nvme.py) and testing GPT parsing separately
3. Boot QEMU with q35 + NVMe (using ISO boot, not UEFI — to avoid OVMF dependency in tests)
4. Verify `[NVME] OK:` and partition detection messages appear
5. Verify shell can access files on the ext2 partition

For CI-friendliness (no sudo), the test can:
- Create a raw GPT image using `sgdisk` (which works on regular files)
- Create ext2 on the second partition using `mke2fs -F -E offset=<bytes>`
- Boot via ISO + NVMe disk attached

```python
#!/usr/bin/env python3
"""test_disk.py — verify GPT partition detection and ext2 mount from NVMe disk."""

import subprocess, sys, os, tempfile

BOOT_TIMEOUT = 120
BUILD = os.path.join(os.path.dirname(__file__), '..', 'build')
ISO = os.path.join(BUILD, 'aegis.iso')

def create_gpt_disk(path):
    """Create a GPT disk with two partitions using sgdisk."""
    # Create 64MB image
    subprocess.run(['dd', 'if=/dev/zero', f'of={path}', 'bs=1M', 'count=64'],
                   check=True, capture_output=True)
    # Create GPT
    subprocess.run(['sgdisk', '-Z', path], check=True, capture_output=True)
    subprocess.run(['sgdisk', '-n', '1:2048:+8M', '-t', '1:ef00', path],
                   check=True, capture_output=True)
    subprocess.run(['sgdisk', '-n', '2:0:0', '-t', '2:8300', path],
                   check=True, capture_output=True)
    # Format partition 2 as ext2 (offset calculation: partition 2 starts after
    # 2048 + 8M/512 = 2048 + 16384 = 18432 sectors = 18432*512 bytes offset)
    # Get actual offset from sgdisk
    result = subprocess.run(['sgdisk', '-i', '2', path],
                            check=True, capture_output=True, text=True)
    # Parse "First sector:" line
    for line in result.stdout.splitlines():
        if 'First sector:' in line:
            start_sector = int(line.split(':')[1].strip().split()[0])
            break
    offset = start_sector * 512
    subprocess.run(['mke2fs', '-t', 'ext2', '-F',
                    '-E', f'offset={offset}',
                    path, f'{(64*1024*1024 - offset) // 1024}k'],
                   check=True, capture_output=True)

def main():
    if not os.path.exists(ISO):
        print("FAIL: build/aegis.iso not found")
        sys.exit(1)

    with tempfile.NamedTemporaryFile(suffix='.img', delete=False) as f:
        disk_path = f.name

    try:
        create_gpt_disk(disk_path)

        qemu_cmd = [
            'qemu-system-x86_64',
            '-machine', 'q35', '-cpu', 'Broadwell',
            '-cdrom', ISO, '-boot', 'order=d',
            '-drive', f'file={disk_path},if=none,id=nvme0',
            '-device', 'nvme,drive=nvme0,serial=aegis0',
            '-display', 'none', '-vga', 'std',
            '-nodefaults', '-serial', 'stdio',
            '-no-reboot', '-m', '128M',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
        ]

        proc = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        try:
            stdout, _ = proc.communicate(timeout=BOOT_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, _ = proc.communicate()

        output = stdout.decode('utf-8', errors='replace')
        lines = [l for l in output.splitlines() if l.startswith('[')]

        # Verify NVMe init and partition detection
        nvme_ok = any('[NVME] OK:' in l for l in lines)
        part_ok = any('nvme0p' in l for l in lines)

        if nvme_ok and part_ok:
            print("PASS: NVMe + GPT partition detection")
        elif nvme_ok:
            print("FAIL: NVMe OK but no partition detection")
            for l in lines:
                print(l)
            sys.exit(1)
        else:
            print("FAIL: NVMe not initialized")
            for l in lines:
                print(l)
            sys.exit(1)
    finally:
        os.unlink(disk_path)

if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Wire into run_tests.sh**

```bash
echo "--- test_disk ---"
python3 tests/test_disk.py || FAIL=1
```

- [ ] **Step 3: Run**

```bash
python3 tests/test_disk.py
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_disk.py tests/run_tests.sh
git commit -m "tests: add GPT partition detection test (test_disk.py)"
```

---

### Task 5: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 25 to Build Status table**

```markdown
| Installable disk image (Phase 25) | ✅ Done | GPT parse in nvme.c; mkdisk.sh (ESP+ext2); make run-disk (OVMF+NVMe); make install |
```

- [ ] **Step 2: Add new build dependencies to toolchain table**

```markdown
| `grub-efi-amd64-bin` | 2.06+ | GRUB EFI binary for `grub-install --target=x86_64-efi` |
| `ovmf` | — | UEFI firmware for `make run-disk` (OVMF_CODE_4M.fd, OVMF_VARS_4M.fd) |
| `gdisk` | 1.0+ | GPT partitioning (`sgdisk`) for `make disk` |
| `mtools` | 4.x | FAT manipulation (may be required by grub-install) |
| `e2tools` | 0.1+ | ext2 image population (`e2mkdir`, `e2cp`) for `make disk` |
```

- [ ] **Step 3: Add Phase 25 forward-looking constraints**

```markdown
### Phase 25 forward-looking constraints

**`mkdisk.sh` requires sudo.** Loop device mounting and `grub-install` require root. A future phase may use `guestfish` for rootless image creation.

**GPT CRC32 not validated.** `nvme_scan_gpt` checks the "EFI PART" signature but does not verify the header or partition array CRC32 checksums. A corrupt GPT could produce incorrect partition boundaries.

**Max 8 partitions.** The static `part_devs[8]` array limits partition count. GPT supports up to 128 entries.

**OVMF_VARS is per-session.** `mk-ovmf-vars.sh` copies a fresh OVMF_VARS on each `make disk`. UEFI boot order changes are not preserved across image rebuilds.

**No Secure Boot.** GRUB is installed with `--removable` (BOOTX64.EFI fallback path). Secure Boot is not configured and the kernel is not signed.

**`make install` uses raw `dd`.** No partition-aware copy. The entire image including GPT is written. Target disk must be >= 256MB. No safety check on disk size.
```

- [ ] **Step 4: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md build status and dependencies for Phase 25"
```

---

## Final Verification

```bash
make test 2>&1 | tail -5
```

Expected: exit 0 (boot.txt unchanged — no NVMe on `-machine pc`).

```bash
python3 tests/test_disk.py
```

Expected: PASS — GPT partitions detected on q35 with NVMe disk.

```bash
# Full disk boot test (requires OVMF):
make INIT=shell iso && sudo make disk && make run-disk
```

Expected: UEFI → GRUB → Aegis kernel → shell prompt. Display shows framebuffer console (if GRUB sets framebuffer). Serial shows all boot messages including `[NVME] OK:`, `[NVME] partition nvme0p1:`, `[NVME] partition nvme0p2:`, `[EXT2] OK:`.
