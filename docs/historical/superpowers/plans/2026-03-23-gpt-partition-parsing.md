# GPT Partition Parsing — Phase 23 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parse GPT partition tables on NVMe disks at boot, register each partition as a `blkdev_t`, and mount the ext2 root filesystem from `nvme0p1` instead of whole-disk `nvme0`.

**Architecture:** A new `gpt.c` module reads the primary GPT header (LBA 1), validates its CRC32, reads the 32-sector partition entry array in 4 × 8-sector NVMe-safe chunks, and registers each valid partition as a child `blkdev_t` wrapping the parent device with an `lba_offset`. The `make disk` target is rewritten to produce a real GPT-partitioned 64 MB disk image using `sgdisk` + a temp ext2 image spliced in with `dd`. All static buffers — `gpt_scan` is called once at boot so file-scope `static` is safe and avoids stack overflow.

**Tech Stack:** C (kernel), Python 3 (test), QEMU q35, sgdisk (gdisk package), mke2fs + debugfs (e2fsprogs), `dd`

---

## File Map

| File | Action | What changes |
|------|--------|--------------|
| `kernel/fs/gpt.h` | Create | GPT structs + `gpt_scan()` declaration |
| `kernel/fs/gpt.c` | Create | CRC32, header validation, entry scan, blkdev registration |
| `kernel/core/main.c` | Modify (lines 13–14, 66–67) | Add `gpt.h` include + `gpt_scan()` call; `ext2_mount("nvme0p1")` |
| `Makefile` | Modify (lines 100–109, 321–332) | Add `gpt.c` to `FS_SRCS`; rewrite `$(DISK)` rule |
| `tests/test_gpt.py` | Create | GPT + ext2-on-nvme0p1 integration test |
| `tests/run_tests.sh` | Modify (last line area) | Wire in `test_gpt.py` |
| `CLAUDE.md` | Modify (Build Status table) | Mark Phase 23 done; add forward-looking constraints; add sgdisk to toolchain |

`tests/expected/boot.txt` — **no change**. `make test` uses `-machine pc` (no NVMe). `gpt_scan("nvme0")` silently returns 0 when `blkdev_get` returns NULL. Only `test_gpt.py` verifies the `[GPT] OK:` line (on q35 + disk.img).

> **⚠️ SPEC CONFLICT — FOLLOW THIS PLAN, NOT THE SPEC:**
> The spec file lists `tests/expected/boot.txt` as a modified file with `[GPT] OK:` added.
> **That entry in the spec is wrong.** `make test` runs on `-machine pc` with no NVMe controller.
> Adding `[GPT] OK:` to `boot.txt` would permanently break `make test`.
> Do NOT modify `boot.txt`. The `[GPT] OK:` line is verified only by `test_gpt.py`.

---

## Task 1: Write `tests/test_gpt.py` (the failing test — RED)

**Files:**
- Create: `tests/test_gpt.py`

This test will fail until Tasks 2–5 are complete (compilation fails because `gpt.c` doesn't exist yet, and the disk isn't GPT-formatted).

- [ ] **Step 1: Create `tests/test_gpt.py`**

```python
#!/usr/bin/env python3
"""Phase 23 GPT partition parsing test.

Boots the shell ISO with a GPT-partitioned NVMe disk image and verifies:
  1. [GPT] OK: 2 partition(s) found on nvme0 in serial output
  2. [EXT2] OK: line contains "nvme0p1" (partition routing, not whole disk)
  3. Shell prompt appears (ext2 mount on nvme0p1 succeeded)
  4. ls /bin lists expected binaries
  5. cat /etc/motd shows "Welcome to Aegis"
"""
import subprocess, time, sys, os, socket, select, fcntl, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 900
CMD_TIMEOUT  = 120

_KEY_MAP = {
    ' ':  'spc',
    '\n': 'ret',
    '/':  'slash',
    '|':  'shift-backslash',
    '<':  'shift-comma',
    '>':  'shift-dot',
    '&':  'shift-7',
    '.':  'dot',
    '-':  'minus',
    '_':  'shift-minus',
}
for ch in 'abcdefghijklmnopqrstuvwxyz':
    _KEY_MAP[ch] = ch
for ch in '0123456789':
    _KEY_MAP.setdefault(ch, ch)


def _char_to_key(ch):
    return _KEY_MAP.get(ch)


def build_iso():
    real_uid = os.getuid()
    real_gid = os.getgid()
    def drop_euid():
        os.setegid(real_gid)
        os.seteuid(real_uid)
    r = subprocess.run(["make", "INIT=shell", "iso"], preexec_fn=drop_euid)
    if r.returncode != 0:
        print("[FAIL] make INIT=shell iso failed")
        sys.exit(1)


def build_disk():
    """Rebuild disk image — GPT layout required for this test."""
    r = subprocess.run(["make", "disk"])
    if r.returncode != 0:
        print("[FAIL] make disk failed")
        sys.exit(1)


def _send_key(mon_sock, keyname):
    cmd = ("sendkey %s\n" % keyname).encode()
    mon_sock.sendall(cmd)
    time.sleep(0.03)
    try:
        mon_sock.recv(4096)
    except BlockingIOError:
        pass


def _type_string(mon_sock, text):
    for ch in text:
        key = _char_to_key(ch)
        if key is None:
            raise ValueError("No key mapping for character %r" % ch)
        _send_key(mon_sock, key)


def _read_until(proc, deadline, sentinel):
    fd = proc.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            break
        buf += chunk
        if sentinel.encode() in buf:
            return buf.decode(errors="replace")
    return buf.decode(errors="replace")


def run_session(commands):
    """Boot q35 + NVMe, wait for prompt, inject commands, return serial output."""
    mon_path = tempfile.mktemp(suffix=".sock", prefix="aegis_gpt_")
    cmd = [
        QEMU,
        "-machine", "q35", "-cpu", "Broadwell",
        "-cdrom", ISO, "-boot", "order=d",
        "-drive", "file=%s,format=raw,if=none,id=nvme0" % DISK,
        "-device", "nvme,drive=nvme0,serial=aegis00",
        "-display", "none", "-vga", "std",
        "-nodefaults", "-serial", "stdio",
        "-no-reboot", "-m", "128M",
        "-monitor", "unix:%s,server,nowait" % mon_path,
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        if os.path.exists(mon_path):
            break
        time.sleep(0.1)
    else:
        proc.kill()
        raise RuntimeError("QEMU monitor socket never appeared")

    mon_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon_sock.connect(mon_path)
    mon_sock.setblocking(False)

    all_output = []
    boot_out = _read_until(proc, time.time() + BOOT_TIMEOUT, "\n#")
    all_output.append(boot_out)

    for cmd_str in commands:
        _type_string(mon_sock, cmd_str + "\n")
        time.sleep(0.1)
        out = _read_until(proc, time.time() + CMD_TIMEOUT, "\n#")
        all_output.append(out)

    proc.kill()
    proc.wait()
    mon_sock.close()
    try:
        os.unlink(mon_path)
    except FileNotFoundError:
        pass
    return "".join(all_output)


def main():
    # Normalize working directory to project root so relative paths work
    # regardless of where the script is invoked from (same pattern as test_ext2.py)
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    build_iso()
    # Always rebuild disk: this test requires GPT layout, not the old raw ext2
    if os.path.exists(DISK):
        os.unlink(DISK)
    build_disk()

    print("Running GPT partition test...")
    output = run_session(["ls /bin", "cat /etc/motd"])

    passed = True

    # 1. GPT line present
    if "[GPT] OK: 2 partition(s) found on nvme0" not in output:
        print("[FAIL] [GPT] OK line missing or wrong")
        print("Output:\n", output[-2000:])
        passed = False
    else:
        print("[PASS] [GPT] OK: 2 partition(s) found on nvme0")

    # 2. EXT2 mounts nvme0p1, not nvme0
    if "nvme0p1" not in output:
        print("[FAIL] ext2 did not mount nvme0p1")
        passed = False
    else:
        print("[PASS] ext2 mounted nvme0p1")

    # 3. Shell prompt appeared
    if "\n#" not in output:
        print("[FAIL] shell prompt never appeared")
        passed = False
    else:
        print("[PASS] shell prompt appeared")

    # 4. ls /bin lists binaries
    for binary in ["sh", "ls", "cat", "echo"]:
        if binary not in output:
            print("[FAIL] /bin/%s missing from ls output" % binary)
            passed = False
        else:
            print("[PASS] /bin/%s present" % binary)

    # 5. /etc/motd content
    if "Welcome to Aegis" not in output:
        print("[FAIL] /etc/motd content missing")
        passed = False
    else:
        print("[PASS] /etc/motd: Welcome to Aegis")

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify the test exists but does not yet pass**

```bash
# This should fail because gpt.c doesn't exist and disk isn't GPT-formatted.
# We expect: compilation failure or test assertion failure.
# Do NOT fix it yet — just confirm it fails.
python3 tests/test_gpt.py 2>&1 | head -20
```

Expected: compile error (no gpt.c) or `[FAIL]` output.

- [ ] **Step 3: Commit the failing test**

```bash
git add tests/test_gpt.py
git commit -m "test(phase23): add test_gpt.py — failing RED test for GPT partition parsing"
```

---

## Task 2: Create `kernel/fs/gpt.h`

**Files:**
- Create: `kernel/fs/gpt.h`

No behavioral test for a header file — correctness is verified when gpt.c compiles and links.

- [ ] **Step 1: Create `kernel/fs/gpt.h`**

```c
#ifndef GPT_H
#define GPT_H

#include <stdint.h>

/* gpt_scan — scan the GPT on blkdev `devname` and register each valid
 * partition as a child blkdev named "<devname>p<N>" (e.g. "nvme0p1").
 *
 * Returns 0 silently when no blkdev named devname exists (consistent
 * with nvme_init, pcie_init, xhci_init behavior on absent hardware).
 *
 * Prints [GPT] OK: <n> partition(s) found on <devname>
 *     or [GPT] WARN: <reason> on failure after the device is found.
 *
 * Returns the number of partitions registered (0 on failure). */
int gpt_scan(const char *devname);

#endif /* GPT_H */
```

- [ ] **Step 2: Commit**

```bash
git add kernel/fs/gpt.h
git commit -m "feat(phase23): add kernel/fs/gpt.h"
```

---

## Task 3: Create `kernel/fs/gpt.c`

**Files:**
- Create: `kernel/fs/gpt.c`

This is the main implementation. Read carefully — there are several correctness constraints baked into the design.

**Critical constraints (read before touching this code):**
1. `s_full_entries[16384]` is 16 KB — it MUST be `static`. A local variable would overflow the 4 KB kernel stack immediately.
2. `s_devs[]` and `s_parts[]` MUST be `static` — `blkdev_register` stores a raw pointer to the `blkdev_t`. Local variables would become dangling pointers after `gpt_scan()` returns.
3. NVMe driver rejects `count * 512 > 4096` — read 8 sectors max per call. The 32-sector entry array is read in 4 chunks.
4. Use `hdr.partition_entry_lba` (not hardcoded `2`) for the entry array start — needed for backup header fallback.
5. Layout guard (`num_entries > 128 || entry_size != 128`) MUST come before reading the entry array and calling CRC32.
6. CRC32 polynomial: `0xEDB88320` (reflected).
7. `header_crc32` offset in struct is byte 16 (after signature[8] + revision[4] + header_size[4]).

- [ ] **Step 1: Create `kernel/fs/gpt.c`**

```c
#include "gpt.h"
#include "blkdev.h"
#include "../core/printk.h"
#include <stdint.h>
#include <stddef.h>

#define GPT_MAX_PARTS 7   /* BLKDEV_MAX=8 minus 1 slot for the parent disk */

/* ── CRC32 (polynomial 0xEDB88320, reflected) ─────────────────────────────
 * Standard CRC32 used by GPT tools (sgdisk, gdisk, parted).
 * Table is computed on first call and cached in s_crc_table[]. */

static uint32_t s_crc_table[256];
static int      s_crc_ready = 0;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320U : 0U);
        s_crc_table[i] = c;
    }
    s_crc_ready = 1;
}

static uint32_t crc32_compute(const uint8_t *buf, uint32_t len)
{
    if (!s_crc_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ s_crc_table[(crc ^ buf[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFU;
}

/* ── GPT on-disk types (packed, no padding) ───────────────────────────────
 * Structs are used only as overlays on 512-byte sector buffers; they are
 * never stack-allocated directly (header is copied with memcpy). */

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];          /* "EFI PART" */
    uint32_t revision;              /* 0x00010000 */
    uint32_t header_size;           /* typically 92 */
    uint32_t header_crc32;          /* CRC32 with this field zeroed */
    uint32_t reserved;
    uint64_t my_lba;                /* LBA of this header */
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;   /* LBA of partition array */
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];   /* all-zeros = unused entry */
    uint8_t  part_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;         /* inclusive */
    uint64_t attributes;
    uint16_t name[36];        /* UTF-16LE, ignored */
} gpt_entry_t;

/* ── Partition blkdev callbacks ───────────────────────────────────────────
 * Each partition blkdev delegates to its parent, adding lba_offset.
 * lba_offset is stored in gpt_part_priv_t (not blkdev_t.lba_offset)
 * so that the blkdev interface stays zero-based for callers. */

typedef struct {
    blkdev_t *parent;
    uint64_t  lba_offset;  /* partition start LBA on parent */
} gpt_part_priv_t;

/* MUST be static: blkdev_register stores a pointer, not a copy.
 * Local variables would produce dangling pointers after gpt_scan() returns. */
static gpt_part_priv_t s_parts[GPT_MAX_PARTS];
static blkdev_t        s_devs[GPT_MAX_PARTS];

static int gpt_part_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    gpt_part_priv_t *p = (gpt_part_priv_t *)dev->priv;
    return p->parent->read(p->parent, lba + p->lba_offset, count, buf);
}

static int gpt_part_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                          const void *buf)
{
    gpt_part_priv_t *p = (gpt_part_priv_t *)dev->priv;
    return p->parent->write(p->parent, lba + p->lba_offset, count, buf);
}

/* ── Header validation ────────────────────────────────────────────────────
 * sector_buf: 512-byte buffer containing the raw sector read from disk.
 * expected_lba: the LBA at which this header was found (1 for primary,
 *               block_count-1 for backup). Checked against hdr.my_lba. */

static const uint8_t k_gpt_sig[8] = { 'E','F','I',' ','P','A','R','T' };

static int header_valid(const uint8_t *sector_buf, uint64_t expected_lba)
{
    const gpt_header_t *h = (const gpt_header_t *)sector_buf;

    /* Signature */
    for (int i = 0; i < 8; i++)
        if (h->signature[i] != k_gpt_sig[i]) return 0;

    /* Header size: 92 (UEFI minimum) to 512 (one full sector).
     * Upper bound prevents CRC from reading past the sector buffer. */
    if (h->header_size < 92 || h->header_size > 512) return 0;

    /* my_lba must equal the LBA we actually read from */
    if (h->my_lba != expected_lba) return 0;

    /* CRC32 over first header_size bytes with header_crc32 field zeroed.
     * header_crc32 is at byte offset 16 (signature[8]+revision[4]+header_size[4]). */
    uint8_t copy[512];
    __builtin_memcpy(copy, sector_buf, h->header_size);
    copy[16] = copy[17] = copy[18] = copy[19] = 0;   /* zero header_crc32 */
    if (crc32_compute(copy, h->header_size) != h->header_crc32) return 0;

    return 1;
}

/* ── gpt_scan ─────────────────────────────────────────────────────────────
 * Scans devname for a valid GPT, registers partitions as child blkdevs. */

int gpt_scan(const char *devname)
{
    /* All large buffers are static to avoid stack overflow.
     * gpt_scan is called once at boot from a single-threaded context. */
    static uint8_t s_sector[512];           /* one sector (header read) */
    static uint8_t s_entry_chunk[4096];     /* per-chunk read (8 sectors) */
    static uint8_t s_entries[128 * 128];    /* full partition array (16 KB!) */

    blkdev_t *dev = blkdev_get(devname);
    if (!dev) return 0;  /* absent hardware — silent, same as nvme_init */

    /* ── Read + validate primary GPT header (LBA 1) ── */
    if (dev->read(dev, 1, 1, s_sector) < 0) {
        printk("[GPT] WARN: cannot read GPT header on %s\n", devname);
        return 0;
    }

    gpt_header_t hdr;
    __builtin_memcpy(&hdr, s_sector, sizeof(hdr));

    if (!header_valid(s_sector, 1)) {
        /* Primary invalid — try backup header at last LBA */
        uint64_t last_lba = dev->block_count - 1;
        if (dev->read(dev, last_lba, 1, s_sector) < 0 ||
            !header_valid(s_sector, last_lba)) {
            printk("[GPT] WARN: no valid GPT on %s\n", devname);
            return 0;
        }
        __builtin_memcpy(&hdr, s_sector, sizeof(hdr));
    }

    /* ── Validate entry table parameters BEFORE reading the array ──────────
     * Must check here: if num_partition_entries > 128, the 4-chunk loop
     * would cover more than 16 KB, overflowing s_entries[].
     * We only support the standard 128-entry × 128-byte layout. */
    if (hdr.num_partition_entries > 128 || hdr.partition_entry_size != 128) {
        printk("[GPT] WARN: unsupported GPT layout on %s\n", devname);
        return 0;
    }

    /* ── Read partition entry array in 4 × 8-sector chunks ─────────────────
     * NVMe driver rejects count * 512 > 4096 (max 8 sectors per call).
     * 32 sectors / 8 = 4 chunks. Use hdr.partition_entry_lba (not 2) so
     * the backup header's entry array is read correctly on fallback. */
    for (int chunk = 0; chunk < 4; chunk++) {
        uint64_t lba = hdr.partition_entry_lba + (uint64_t)(chunk * 8);
        if (dev->read(dev, lba, 8, s_entry_chunk) < 0) {
            printk("[GPT] WARN: cannot read partition entries on %s\n", devname);
            return 0;
        }
        __builtin_memcpy(s_entries + chunk * 4096, s_entry_chunk, 4096);
    }

    /* ── Validate partition array CRC ──────────────────────────────────────
     * GPT spec: CRC covers num_partition_entries * partition_entry_size bytes,
     * not the full s_entries[] buffer. The guard above ensured entry_size == 128
     * and num_entries <= 128, so the byte count is safe. We read all 32 sectors
     * (128 × 128 bytes) but only feed the header-declared length to CRC. */
    if (crc32_compute(s_entries, hdr.num_partition_entries * 128) !=
        hdr.partition_array_crc32) {
        printk("[GPT] WARN: partition array CRC mismatch on %s\n", devname);
        return 0;
    }

    /* ── Register valid partition entries as child blkdevs ── */
    int part_num = 1;  /* starts at 1 → first name is "nvme0p1" */

    for (int i = 0; i < 128 && part_num <= GPT_MAX_PARTS; i++) {
        const gpt_entry_t *e =
            (const gpt_entry_t *)(s_entries + (uint32_t)i * 128);

        /* Skip empty entries (type_guid all-zeros) */
        int used = 0;
        for (int j = 0; j < 16; j++) { if (e->type_guid[j]) { used = 1; break; } }
        if (!used) continue;
        /* Strict less-than: single-sector partitions unsupported (simplification) */
        if (e->start_lba >= e->end_lba) continue;

        int idx = part_num - 1;

        /* Private data: parent device + partition start LBA */
        s_parts[idx].parent     = dev;
        s_parts[idx].lba_offset = e->start_lba;

        /* blkdev_t: block_count, block_size, zero-based lba_offset, callbacks */
        s_devs[idx].block_count = e->end_lba - e->start_lba + 1;
        s_devs[idx].block_size  = dev->block_size;
        s_devs[idx].lba_offset  = 0;
        s_devs[idx].read        = gpt_part_read;
        s_devs[idx].write       = gpt_part_write;
        s_devs[idx].priv        = &s_parts[idx];

        /* Build name manually — kernel has no snprintf.
         * Copy up to 13 chars of devname, append 'p', append ASCII digit,
         * null-terminate. Supports part_num 1–7 (single digit). */
        int ni = 0;
        for (; devname[ni] != '\0' && ni < 13; ni++)
            s_devs[idx].name[ni] = devname[ni];
        s_devs[idx].name[ni++] = 'p';
        s_devs[idx].name[ni++] = '0' + (char)part_num;
        s_devs[idx].name[ni]   = '\0';

        if (blkdev_register(&s_devs[idx]) < 0) {
            printk("[GPT] WARN: blkdev table full, %d partition(s) registered\n",
                   part_num - 1);
            break;
        }
        part_num++;
    }

    int count = part_num - 1;
    if (count == 0) {
        printk("[GPT] WARN: no valid GPT on %s\n", devname);
        return 0;
    }

    printk("[GPT] OK: %d partition(s) found on %s\n", count, devname);
    return count;
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/fs/gpt.c
git commit -m "feat(phase23): add kernel/fs/gpt.c — CRC32, header validation, partition scan"
```

---

## Task 4: Wire `gpt.c` into Makefile + `main.c`; verify `make test` stays GREEN

**Files:**
- Modify: `Makefile` (lines 100–109 — FS_SRCS block)
- Modify: `kernel/core/main.c` (lines 13–14, 66–67)

After this task `make test` must still exit 0. `gpt_scan("nvme0")` returns silently on `-machine pc` (no NVMe device). Only `test_gpt.py` sees the `[GPT] OK:` line.

- [ ] **Step 1: Add `kernel/fs/gpt.c` to `FS_SRCS` in Makefile**

Find the `FS_SRCS` block (around line 100):

```makefile
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
```

Add `kernel/fs/gpt.c \` before `kernel/fs/ext2.c \`.

- [ ] **Step 2: Add `#include "../fs/gpt.h"` to `kernel/core/main.c`**

After line 14 (`#include "../fs/ext2.h"`):

```c
#include "../fs/gpt.h"
```

- [ ] **Step 3: Add `gpt_scan("nvme0")` call and update `ext2_mount` in `main.c`**

Replace lines 66–67:

```c
    nvme_init();            /* NVMe block device — [NVME] OK or silent skip  */
    ext2_mount("nvme0");    /* mount ext2 root — [EXT2] OK or silent (-1)   */
```

With:

```c
    nvme_init();            /* NVMe block device — [NVME] OK or silent skip  */
    gpt_scan("nvme0");      /* GPT partitions — [GPT] OK or silent (no NVMe) */
    ext2_mount("nvme0p1");  /* mount partition 1 — [EXT2] OK or silent (-1)  */
```

- [ ] **Step 4: Build and verify `make test` still passes**

```bash
make test
```

Expected: exits 0. The `-machine pc` boot has no NVMe, so `gpt_scan` returns silently and `ext2_mount("nvme0p1")` also fails silently. `boot.txt` is unchanged — the diff still matches.

- [ ] **Step 5: Commit**

```bash
git add Makefile kernel/core/main.c
git commit -m "feat(phase23): wire gpt_scan into main.c + Makefile; make test GREEN"
```

---

## Task 5: Rewrite `make disk` for GPT layout; rebuild disk; verify `test_gpt.py` passes

**Files:**
- Modify: `Makefile` (lines 321–333 — disk target)

The current disk target creates a raw ext2 image. We replace it with a GPT-partitioned 64 MB image using `sgdisk`, then populate the ext2 partition via a temp image.

**New build dependency:** `sgdisk` — install with `apt install gdisk` if not present.

Check first:
```bash
sgdisk --version 2>/dev/null || echo "MISSING: apt install gdisk"
```

- [ ] **Step 1: Replace the disk target in `Makefile`**

Two separate edits are required. Do NOT combine them:

**Edit A** — Insert the following variable definitions immediately after the existing `DISK = $(BUILD)/disk.img` line (~line 321), before the existing `disk: $(DISK)` line. These are Makefile-scope assignments (not shell commands):

```makefile
SGDISK     = sgdisk
# nvme0p1: LBA 34–122879 = 122846 sectors = ~60 MB
P1_SECTORS = 122846

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
```

**Edit B** — Replace the existing `$(DISK): user/shell/shell.elf` rule and its recipe (from that line through the final `@echo "Disk image created..."` line) with:

```makefile
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
	dd if=/tmp/aegis-p1.img of=$(DISK) bs=512 seek=34 conv=notrunc 2>/dev/null
	@rm -f /tmp/aegis-p1.img /tmp/aegis-motd
	@echo "Disk image created: $(DISK)"
```

- [ ] **Step 2: Remove the old disk.img and rebuild**

```bash
rm -f build/disk.img
make INIT=shell iso disk
```

Expected:
- ISO builds successfully
- `sgdisk` creates GPT with 2 partitions
- `mke2fs` formats `/tmp/aegis-p1.img`
- `debugfs` populates `/bin`, `/etc/motd`
- `dd` splices the ext2 image at LBA 34
- Output ends with `Disk image created: build/disk.img`

- [ ] **Step 3: Verify GPT layout with sgdisk**

```bash
sgdisk -p build/disk.img
```

Expected output includes:
```
Number  Start (sector)    End (sector)  Size       Code  Name
   1              34          122879   60.0 MiB    8300  aegis-root
   2          122880          131038    4.0 MiB    8200  aegis-swap
```

- [ ] **Step 4: Run `test_gpt.py` — verify GREEN**

```bash
python3 tests/test_gpt.py
```

Expected: all checks pass:
```
[PASS] [GPT] OK: 2 partition(s) found on nvme0
[PASS] ext2 mounted nvme0p1
[PASS] shell prompt appeared
[PASS] /bin/sh present
[PASS] /bin/ls present
[PASS] /bin/cat present
[PASS] /bin/echo present
[PASS] /etc/motd: Welcome to Aegis
```

- [ ] **Step 5: Verify `make test` still passes (regression check)**

```bash
make test
```

Expected: exits 0. (Rebuilds ISO — the `-machine pc` test still uses no NVMe.)

- [ ] **Step 6: Verify `test_ext2.py` still passes (regression check)**

```bash
rm -f build/disk.img
python3 tests/test_ext2.py
```

Expected: PASS. The ext2 test writes/reads `/tmp/test.txt` across two boots; after Phase 23 the kernel mounts `nvme0p1` instead of `nvme0`, but the data is still there.

- [ ] **Step 7: Wire `test_gpt.py` into `tests/run_tests.sh`**

Add at the end of `tests/run_tests.sh`, after the `test_xhci` block:

```bash
# Phase 23 GPT partition parsing test — boots on q35 with GPT-partitioned
# NVMe disk image; verifies [GPT] OK, [EXT2] nvme0p1, shell prompt, /bin, /etc/motd.
echo "--- test_gpt ---"
python3 tests/test_gpt.py
```

- [ ] **Step 8: Commit**

```bash
git add Makefile tests/run_tests.sh
git commit -m "feat(phase23): rewrite make disk for GPT layout; wire test_gpt.py"
```

---

## Task 6: Update `CLAUDE.md`

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add `sgdisk` to the Build Toolchain table**

Find the toolchain table and add after the `musl-gcc` row:

```markdown
| `sgdisk` (gdisk) | 1.0.x+ | GPT partition table creation for `make disk` |
```

- [ ] **Step 2: Add Phase 23 row to Build Status table**

Find the `| xHCI + USB HID keyboard (Phase 22)` row and add after it:

```markdown
| GPT partition parsing (Phase 23) | ✅ Done | gpt.c CRC32 + header validation; nvme0p1/nvme0p2 registered; ext2 mounts nvme0p1; test_gpt.py PASS |
```

- [ ] **Step 3: Add Phase 23 forward-looking constraints section**

After the Phase 22 constraints section, add:

```markdown
### Phase 23 forward-looking constraints

**Partition LBA bounds are not enforced on reads/writes.** `gpt_part_read` and
`gpt_part_write` add `lba_offset` but do not clamp LBA + count to the partition's
`block_count`. A buggy caller could read past the partition boundary into adjacent
structures. Add bounds checking when the blkdev interface gains a per-device bounds hook.

**`nvme0p2` is registered but not mounted.** It appears as a blkdev but no
filesystem driver uses it. A future phase can format and mount it as swap or a
second ext2 volume.

**Only the primary GPT header is used.** When the primary is valid, the backup is
not checked for consistency. The UEFI spec recommends verifying both headers match;
deferred to a future hardening phase.

**Partition count capped at `GPT_MAX_PARTS = 7`.** With `BLKDEV_MAX = 8` and one
slot for the parent disk. Raise both constants together if more partitions are needed.

**`make disk` rebuilds entirely when any user binary changes.** The `$(DISK)` rule
lists all user ELFs as prerequisites. A change to any binary triggers a full disk
rebuild including running `sgdisk`. This is correct but slow. A future optimization
could separate partition table creation (once) from filesystem population (on change).
```

- [ ] **Step 4: Update the last-updated timestamp**

Add at the end of the last-updated lines:

```
*Last updated: 2026-03-23 — Phase 23 complete, test_gpt.py PASS. GPT partition parsing; nvme0p1/nvme0p2 registered; ext2 mounts nvme0p1; make disk rewritten for GPT layout with sgdisk; make test GREEN.*
```

- [ ] **Step 5: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs(phase23): update CLAUDE.md — Phase 23 complete, sgdisk toolchain, forward-looking constraints"
```

---

## Final Verification

- [ ] Run the full test suite:

```bash
make test
```

Expected: exits 0.

- [ ] Run `test_gpt.py` standalone to confirm:

```bash
python3 tests/test_gpt.py
```

Expected: all `[PASS]` lines.

- [ ] Confirm disk layout:

```bash
sgdisk -p build/disk.img
```

Expected: 2 partitions (nvme0p1 at LBA 34–122879, nvme0p2 at 122880–131038).
