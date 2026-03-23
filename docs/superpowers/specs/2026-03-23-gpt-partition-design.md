# GPT Partition Parsing — Aegis Phase 23 Design Spec

## Goal

Parse GPT partition tables on NVMe disks at boot, register each partition as an
independent `blkdev_t` with a correct `lba_offset`, and mount the ext2 root
filesystem from `nvme0p1` instead of the whole-disk `nvme0` device.

## Background and Constraints

### Current state

- `nvme_init()` registers a single `blkdev_t` named `"nvme0"` with `lba_offset = 0`
  (whole disk). The `lba_offset` field already exists in `blkdev_t` for exactly
  this purpose.
- `ext2_mount("nvme0")` reads the superblock at LBA 2, assuming LBA 0 is the
  start of the ext2 filesystem. This assumption is broken by a GPT disk because
  LBA 0–33 are reserved for the protective MBR and partition table.
- The disk image (`build/disk.img`) is currently an unpartitioned raw ext2
  filesystem — no GPT header, no protective MBR.
- `blkdev_t.name` is 16 bytes — long enough for `"nvme0p15\0"` (NVMe naming
  convention supports up to 15 partitions before requiring a `p` separator; we
  support up to 8 blkdevs total, so up to 7 partitions per disk is the practical
  limit).
- `BLKDEV_MAX = 8` — room for the parent disk plus several partitions.

### What does NOT change

- `blkdev_t`, `blkdev_register()`, `blkdev_get()` — no changes.
- `nvme_blkdev_read` / `nvme_blkdev_write` callbacks — no changes. The existing
  callbacks already receive `lba` relative to the device start; they are unaware
  of `lba_offset`. Partition devices need wrapper callbacks that add `lba_offset`
  before delegating.
- `ext2.c` internals — no changes beyond replacing `"nvme0"` with `"nvme0p1"` in
  `main.c`'s `ext2_mount()` call.
- Boot sequence serial output format — `[GPT] OK:` or `[GPT] WARN:` lines follow
  the project standard.

---

## GPT On-Disk Format (what we parse)

### LBA layout

| LBA | Content |
|-----|---------|
| 0 | Protective MBR (ignored) |
| 1 | Primary GPT header (92 bytes) |
| 2–33 | Partition entry array (128 entries × 128 bytes = 16384 bytes = 32 sectors) |
| 34 | Start of usable space (first partition typically starts here) |
| last−33 | Secondary partition entry array |
| last | Backup GPT header |

### GPT Header (LBA 1, 92 bytes significant)

```c
typedef struct __attribute__((packed)) {
    uint8_t  signature[8];    /* "EFI PART" = 0x5452415020494645 */
    uint32_t revision;        /* 0x00010000 = version 1.0 */
    uint32_t header_size;     /* usually 92 */
    uint32_t header_crc32;    /* CRC32 of header with this field zeroed */
    uint32_t reserved;        /* must be zero */
    uint64_t my_lba;          /* LBA of this header (1 for primary) */
    uint64_t alternate_lba;   /* LBA of backup header */
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];   /* ignored */
    uint64_t partition_entry_lba; /* LBA of partition array (2 for primary) */
    uint32_t num_partition_entries; /* typically 128 */
    uint32_t partition_entry_size;  /* typically 128 */
    uint32_t partition_array_crc32; /* CRC32 of entire partition array */
} gpt_header_t;
```

Validation: check `signature`, check `header_size >= 92`, compute CRC32 of the
header (with `header_crc32` zeroed during computation) and compare. If invalid,
try backup header at the last LBA. If both fail, emit `[GPT] WARN:` and return.

### GPT Partition Entry (128 bytes each)

```c
typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];   /* all-zeros = unused entry */
    uint8_t  part_guid[16];   /* ignored */
    uint64_t start_lba;
    uint64_t end_lba;         /* inclusive */
    uint64_t attributes;      /* ignored */
    uint16_t name[36];        /* UTF-16LE, ignored */
} gpt_entry_t;
```

An entry is valid if `type_guid` is not all-zeros and `start_lba < end_lba`.

### CRC32 algorithm

Standard CRC32 (polynomial 0xEDB88320, reflected). A 256-entry lookup table
computed at first call (or compile time). Input is the byte sequence; output
is the 32-bit checksum. Must exactly match the standard `crc32` function used
by GPT tools (`sgdisk`, `gdisk`, `parted`).

---

## Disk Image Layout (two partitions)

| Region | LBAs | Size | Content |
|--------|------|------|---------|
| Protective MBR | 0 | 512 B | Created by sgdisk |
| Primary GPT header | 1 | 512 B | Created by sgdisk |
| Partition entry array | 2–33 | 16 KB | 128 entries |
| nvme0p1 (ext2 root) | 34–122879 | ~60 MB | ext2 filesystem |
| nvme0p2 (reserved) | 122880–131038 | ~4 MB | Future swap / data |
| Secondary GPT | 131039–131071 | 16.5 KB | Backup header + entries |

Total disk: 64 MB (131072 × 512-byte sectors).

`make disk` uses `sgdisk` to create the partition table, then `mke2fs` to format
partition 1, then `debugfs` to populate it. `sgdisk` is part of the `gdisk`
package (`apt install gdisk`).

---

## File Structure

### New files

| File | Responsibility |
|------|----------------|
| `kernel/fs/gpt.h` | `gpt_header_t`, `gpt_entry_t`, `gpt_scan()` declaration |
| `kernel/fs/gpt.c` | CRC32 table, header validation, entry enumeration, blkdev registration |

### Modified files

| File | Change |
|------|--------|
| `kernel/core/main.c` | Call `gpt_scan("nvme0")` after `nvme_init()`; change `ext2_mount("nvme0")` to `ext2_mount("nvme0p1")` |
| `Makefile` | Add `gpt.c` to `FS_SRCS`; rewrite `disk` target using `sgdisk` |
| `tests/expected/boot.txt` | Add `[GPT] OK:` line after `[NVME]` line |

### No changes needed

- `blkdev.h`, `blkdev.c` — existing interface is sufficient
- `nvme.c` — no changes to driver or callbacks
- `ext2.c`, `ext2_cache.c`, `ext2_dir.c` — no changes
- `vfs.c` — no changes

---

## Component Design

### `gpt.h`

```c
#ifndef GPT_H
#define GPT_H
#include <stdint.h>

/* gpt_scan — scan the GPT on blkdev `devname` and register each valid
 * partition as a child blkdev named "<devname>p<N>" (e.g. "nvme0p1").
 *
 * Prints [GPT] OK: <n> partition(s) found on <devname>
 *     or [GPT] WARN: no valid GPT on <devname>
 *
 * Returns the number of partitions registered (0 on failure). */
int gpt_scan(const char *devname);

#endif /* GPT_H */
```

### `gpt.c` — structure

```
crc32_table[256]          static, computed on first call
crc32(buf, len)           standard reflected CRC32

gpt_scan(devname):
  dev = blkdev_get(devname)              // find parent blkdev
  read LBA 1 into buf[512]              // primary GPT header
  parse buf into gpt_header_t hdr
  if !header_valid(&hdr, 1):
    read last LBA into buf              // try backup header
    parse into hdr
    if !header_valid(&hdr, last_lba): WARN + return 0
  read partition entry array (LBAs 2..33) into entries[128]
  validate partition_array_crc32
  for each entry[i] where type_guid != all-zeros and start < end:
    allocate gpt_part_t from static pool
    set .parent = dev, .lba_offset = entry.start_lba
    set .block_count = entry.end_lba - entry.start_lba + 1
    set .block_size = dev->block_size
    snprintf name: devname + "p" + (part_num)   // "nvme0p1", "nvme0p2"
    set .read = gpt_part_read, .write = gpt_part_write
    blkdev_register(...)
    part_num++
  printk("[GPT] OK: %d partition(s) found on %s\n", part_num-1, devname)
  return part_num - 1
```

### Partition blkdev callbacks

```c
typedef struct {
    blkdev_t *parent;
    uint64_t  lba_offset;   /* partition start on parent device */
} gpt_part_priv_t;

static int gpt_part_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    gpt_part_priv_t *p = dev->priv;
    return p->parent->read(p->parent, lba + p->lba_offset, count, buf);
}

static int gpt_part_write(blkdev_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    gpt_part_priv_t *p = dev->priv;
    return p->parent->write(p->parent, lba + p->lba_offset, count, buf);
}
```

`lba_offset` on the `blkdev_t` struct itself is kept as 0 for partition devices
(it describes the device's own coordinate space). The actual offset is stored in
`gpt_part_priv_t` and applied in the callbacks. This keeps the blkdev interface
consistent: callers always use LBAs starting from 0 relative to the device.

**Static allocation:** `gpt_part_priv_t` and the partition `blkdev_t` structs are
allocated from a static pool of size `GPT_MAX_PARTS = 7` (leaves one blkdev slot
for the parent). No `kva_alloc_pages` needed — these are small fixed-size structs.

### `main.c` changes

```c
nvme_init();             /* [NVME] OK */
gpt_scan("nvme0");       /* [GPT] OK: 2 partition(s) found on nvme0 */
/* ... */
ext2_mount("nvme0p1");   /* mounts partition 1, not whole disk */
```

### `boot.txt` oracle

After `[NVME] OK:` line, add:
```
[GPT] OK: 2 partition(s) found on nvme0
```

The exact wording must match `gpt_scan()`'s `printk` call.

---

## `make disk` rewrite

```makefile
SGDISK = sgdisk
DISK   = $(BUILD)/disk.img
DISK_SIZE_MB = 64

disk: $(DISK)

$(DISK):
	@mkdir -p $(BUILD)
	# Create 64 MB zero image
	dd if=/dev/zero of=$(DISK) bs=1M count=$(DISK_SIZE_MB) 2>/dev/null
	# Create GPT: partition 1 = ext2 root (~60MB), partition 2 = reserved (~4MB)
	$(SGDISK) \
	    --new=1:34:122879   --typecode=1:8300 --change-name=1:aegis-root \
	    --new=2:122880:0    --typecode=2:8200 --change-name=2:aegis-swap \
	    $(DISK)
	# Format partition 1 as ext2 (using sector offset for mke2fs)
	mke2fs -t ext2 -F -L aegis-root \
	    -E offset=$$((34 * 512)) $(DISK) $$((122846))
	# Populate the filesystem
	echo "mkdir /bin\nmkdir /etc\nmkdir /tmp\nmkdir /home" \
	    | debugfs -w -o offset=$$((34 * 512)) $(DISK)
	@printf "Welcome to Aegis\n" > /tmp/aegis-motd
	echo "write user/shell/shell.elf /bin/sh\nwrite /tmp/aegis-motd /etc/motd" \
	    | debugfs -w -o offset=$$((34 * 512)) $(DISK)
	@echo "Disk image created: $(DISK)"
```

Note: `mke2fs -E offset=N` formats a partition within a whole-disk image.
`debugfs -o offset=N` accesses the filesystem at that byte offset. Both tools
support this workflow without loopback devices or root privileges.

---

## Error Handling

| Condition | Response |
|-----------|----------|
| `blkdev_get(devname)` returns NULL | `[GPT] WARN: device not found` + return 0 |
| LBA 1 read fails | `[GPT] WARN: cannot read GPT header` + return 0 |
| Primary header signature/CRC invalid | Try backup header; if backup also invalid: WARN + return 0 |
| Partition array CRC invalid | `[GPT] WARN: partition array CRC mismatch` + return 0 |
| `blkdev_register()` returns -1 (table full) | Stop registering, log how many succeeded |
| Zero valid partition entries | `[GPT] WARN: no valid GPT on <devname>` + return 0 |

The kernel continues booting in all error cases. A GPT failure is non-fatal as
long as it's caught before `ext2_mount("nvme0p1")` — which will fail with its
own error if `nvme0p1` was never registered.

---

## Testing

### `make test` (boot oracle)

Add `[GPT] OK: 2 partition(s) found on nvme0` to `tests/expected/boot.txt`
between the `[NVME]` and `[EXT2]` lines.

### `test_gpt.py` (new test script)

```python
# Boot QEMU with the GPT-partitioned disk image and verify:
# 1. [GPT] OK: 2 partition(s) found on nvme0 appears in serial output
# 2. Shell prompt appears (ext2 mount on nvme0p1 succeeded)
# 3. Basic shell commands work (ls, cat /etc/motd)
```

Wire `test_gpt.py` into `tests/run_tests.sh`.

The existing `test_ext2.py` (ext2 persistence test) continues to pass unchanged —
it writes to the ext2 filesystem and verifies data survives a reboot. It tests the
same path as before, just now via `nvme0p1` instead of `nvme0`.

---

## New Build Dependency

`sgdisk` from the `gdisk` package. Install: `apt install gdisk`.

Add to CLAUDE.md build toolchain table.

---

## Forward-Looking Constraints

**Partition LBA bounds are not enforced on reads/writes.** `gpt_part_read` and
`gpt_part_write` add `lba_offset` but do not clamp the LBA to the partition's
`block_count`. A buggy caller could read past the end of the partition into the
next partition or GPT backup structures. Add bounds checking when the blkdev
interface gains a `lba_bounds_check` hook.

**`nvme0p2` is registered but not mounted.** It appears as a blkdev but no
filesystem driver uses it. Future phases can mount it as swap or a secondary ext2.

**Only the primary GPT header is used.** If the primary header is valid, the
backup is not read. A future phase could verify consistency between primary and
backup headers (GPT spec recommends this).

**Partition count capped at `GPT_MAX_PARTS = 7`.** With `BLKDEV_MAX = 8` and one
slot for the parent disk, 7 partitions is the maximum. A future phase increasing
`BLKDEV_MAX` can raise `GPT_MAX_PARTS` accordingly.
