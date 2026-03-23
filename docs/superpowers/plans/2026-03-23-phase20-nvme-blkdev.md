# Phase 20: NVMe Driver + Block Device Interface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement an NVMe block driver that discovers the NVMe controller via PCIe, initializes admin and I/O queues, and registers a `blkdev_t` for use by the ext2 filesystem in Phase 21.

**Architecture:** A generic block device interface (`kernel/fs/blkdev.h`) provides read/write callbacks decoupled from any specific storage driver. The NVMe driver (`kernel/drivers/nvme.c`) finds the controller via `pcie_find_device(0x01, 0x08, 0x02)`, maps BAR0 for register access, sets up admin and I/O submission/completion queue pairs, issues Identify commands to discover the namespace geometry, and registers itself as `"nvme0"` via `blkdev_register()`. All I/O is synchronous doorbell+poll — no interrupts.

**Tech Stack:** C, NVMe 1.4 spec (admin + I/O command set), PCIe BAR0 MMIO, KVA allocator for queue memory, QEMU `-device nvme` for testing.

**Spec:** docs/superpowers/specs/2026-03-23-aegis-v1-design.md — Phase 20

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/fs/blkdev.h` | Create | `blkdev_t` struct, `blkdev_register()`, `blkdev_get()` |
| `kernel/fs/blkdev.c` | Create | Static table of up to 8 registered block devices |
| `kernel/drivers/nvme.h` | Create | NVMe register structures, command/completion entry types, `nvme_init()` |
| `kernel/drivers/nvme.c` | Create | Admin queue setup, Identify, I/O queue creation, read/write, blkdev registration |
| `kernel/core/main.c` | Modify | Call `nvme_init()` after `pcie_init()` |
| `Makefile` | Modify | Add DRIVER_SRCS with nvme.c; add blkdev.c to FS_SRCS; add `make disk` target; update `make run` with NVMe flags |
| `tests/test_nvme.py` | Create | Boot with NVMe disk, verify `[NVME] OK:` in serial output |
| `tests/run_tests.sh` | Modify | Wire in test_nvme.py |

---

### Task 1: Create blkdev.h and blkdev.c

**Files:**
- Create: `kernel/fs/blkdev.h`
- Create: `kernel/fs/blkdev.c`

- [ ] **Step 1: Create blkdev.h**

```c
/* blkdev.h — Block device abstraction layer
 *
 * Provides a uniform read/write interface for block storage.
 * NVMe, AHCI, virtio-blk, etc. register themselves here.
 * Filesystems (ext2) hold a blkdev_t pointer.
 */
#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

typedef struct blkdev {
    char     name[16];          /* e.g. "nvme0", "nvme0p1" */
    uint64_t block_count;       /* total sectors (512-byte) or blocks */
    uint32_t block_size;        /* 512 or 4096 */
    uint64_t lba_offset;        /* partition start LBA (0 for whole disk) */
    int (*read) (struct blkdev *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *dev, uint64_t lba, uint32_t count, const void *buf);
    void    *priv;              /* driver-private data */
} blkdev_t;

#define BLKDEV_MAX 8

/* Register a block device. Returns 0 on success, -1 if table full. */
int       blkdev_register(blkdev_t *dev);

/* Look up a block device by name. Returns NULL if not found. */
blkdev_t *blkdev_get(const char *name);

#endif /* BLKDEV_H */
```

- [ ] **Step 2: Create blkdev.c**

```c
/* blkdev.c — Block device registration table */
#include "blkdev.h"
#include <stddef.h>
#include <string.h>

static blkdev_t *s_devices[BLKDEV_MAX];
static int        s_count = 0;

int blkdev_register(blkdev_t *dev)
{
    if (s_count >= BLKDEV_MAX || dev == NULL)
        return -1;
    s_devices[s_count++] = dev;
    return 0;
}

blkdev_t *blkdev_get(const char *name)
{
    int i;
    for (i = 0; i < s_count; i++) {
        if (strcmp(s_devices[i]->name, name) == 0)
            return s_devices[i];
    }
    return NULL;
}
```

- [ ] **Step 3: Build to verify no errors**

```bash
make 2>&1 | head -40
```

blkdev.c is not yet in the Makefile — it will be added in Task 3. Verify existing build is not broken.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/blkdev.h kernel/fs/blkdev.c
git commit -m "fs: add block device abstraction layer (blkdev.h + blkdev.c)"
```

---

### Task 2: Create nvme.h and nvme.c

**Files:**
- Create: `kernel/drivers/nvme.h`
- Create: `kernel/drivers/nvme.c`

- [ ] **Step 1: Create nvme.h**

Define NVMe controller register layout, submission queue entry (64 bytes), completion queue entry (16 bytes), admin command opcodes, and I/O command opcodes.

```c
/* nvme.h — NVMe controller driver
 *
 * Implements NVMe 1.4 admin + I/O command sets.
 * Synchronous doorbell+poll I/O — no interrupts.
 */
#ifndef NVME_H
#define NVME_H

#include <stdint.h>

/* NVMe controller registers (BAR0 MMIO, 64-bit) */
typedef struct __attribute__((packed)) {
    uint64_t cap;       /* 0x00: Controller Capabilities */
    uint32_t vs;        /* 0x08: Version */
    uint32_t intms;     /* 0x0C: Interrupt Mask Set */
    uint32_t intmc;     /* 0x10: Interrupt Mask Clear */
    uint32_t cc;        /* 0x14: Controller Configuration */
    uint32_t reserved;  /* 0x18 */
    uint32_t csts;      /* 0x1C: Controller Status */
    uint32_t nssr;      /* 0x20: NVM Subsystem Reset */
    uint32_t aqa;       /* 0x24: Admin Queue Attributes */
    uint64_t asq;       /* 0x28: Admin SQ Base Address */
    uint64_t acq;       /* 0x30: Admin CQ Base Address */
} nvme_regs_t;

/* Submission Queue Entry — 64 bytes */
typedef struct __attribute__((packed)) {
    uint32_t cdw0;      /* Command Dword 0: opcode[7:0], fuse[9:8], psdt[15:14], cid[31:16] */
    uint32_t nsid;      /* Namespace ID */
    uint64_t reserved;
    uint64_t mptr;      /* Metadata Pointer */
    uint64_t prp1;      /* PRP Entry 1 */
    uint64_t prp2;      /* PRP Entry 2 (or PRP list pointer) */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

/* Completion Queue Entry — 16 bytes */
typedef struct __attribute__((packed)) {
    uint32_t dw0;       /* Command-specific */
    uint32_t dw1;       /* Reserved */
    uint16_t sq_head;   /* SQ Head Pointer */
    uint16_t sq_id;     /* SQ Identifier */
    uint16_t cid;       /* Command Identifier */
    uint16_t status;    /* Status Field: bit 0 = phase tag, bits [15:1] = status */
} nvme_cqe_t;

/* Admin command opcodes */
#define NVME_ADMIN_IDENTIFY          0x06
#define NVME_ADMIN_CREATE_IO_CQ      0x05
#define NVME_ADMIN_CREATE_IO_SQ      0x01

/* I/O command opcodes */
#define NVME_IO_READ                 0x02
#define NVME_IO_WRITE                0x01

/* Controller Configuration bits */
#define NVME_CC_EN                   (1 << 0)
#define NVME_CC_CSS_NVM              (0 << 4)
#define NVME_CC_MPS_4K               (0 << 7)   /* 2^(12+0) = 4096 */
#define NVME_CC_IOSQES               (6 << 16)  /* 2^6 = 64 bytes */
#define NVME_CC_IOCQES               (4 << 20)  /* 2^4 = 16 bytes */

/* Controller Status bits */
#define NVME_CSTS_RDY                (1 << 0)

/* Queue sizes */
#define NVME_ADMIN_QUEUE_SIZE        64
#define NVME_IO_QUEUE_SIZE           64    /* 64×64B = 4096B = 1 page; single pmm_alloc_page() */

/* Initialize NVMe: find controller via PCIe, set up queues, register blkdev.
 * Prints [NVME] OK or skip message. Safe to call when no NVMe device present. */
void nvme_init(void);

#endif /* NVME_H */
```

- [ ] **Step 2: Create nvme.c**

Implement the full NVMe init sequence:
1. Find PCIe device class 0x01 / subclass 0x08 / progif 0x02
2. Map BAR0 into kernel VA via `kva_alloc_pages` + `vmm_map_page`
3. Disable controller (CC.EN=0), wait CSTS.RDY=0
4. Allocate Admin SQ (64 entries x 64 bytes) + Admin CQ (64 entries x 16 bytes)
5. Set AQA, ASQ, ACQ; set CC.EN=1, wait CSTS.RDY=1
6. Issue Identify Controller (admin command opcode 0x06, CNS=1)
7. Issue Identify Namespace (NSID=1, CNS=0) — read LBA format, namespace size
8. Create I/O CQ (admin opcode 0x05) + I/O SQ (admin opcode 0x01)
9. Implement `nvme_read_blocks()` and `nvme_write_blocks()` for blkdev callbacks
10. Register `blkdev_t` as `"nvme0"`

Key implementation notes:
- Doorbell stride: `(CAP >> 32) & 0xF` gives DSTRD; doorbell offset = `0x1000 + (2*qid + tail_or_head) * (4 << DSTRD)`
- `sfence` before every doorbell write (store fence ensures SQ entry is visible)
- CQE polling: use `volatile` read of `cqe->status` and check `(status & 1) != expected_phase`
- Phase tag flips every time the CQ wraps around
- All queue memory must be physically contiguous and page-aligned — use `pmm_alloc_page()` for single-page queues, or `kva_alloc_pages()` for larger ones
- Admin SQ: 64 * 64 = 4096 bytes = 1 page; Admin CQ: 64 * 16 = 1024 bytes = 1 page
- I/O SQ: 64 * 64 = 4096 bytes = 1 page (256 entries would require 4 contiguous pages which pmm_alloc_page() cannot guarantee); I/O CQ: 64 * 16 = 1024 bytes = 1 page
- Use `pmm_alloc_page()` for each queue (all queues fit in 1 page at 64-entry depth); map them into kernel VA with `vmm_map_page()`
- The Identify data buffer is 4096 bytes (1 page)

- [ ] **Step 3: Build to verify header correctness**

```bash
make 2>&1 | head -40
```

Not yet in Makefile — verify existing build unchanged.

- [ ] **Step 4: Commit**

```bash
git add kernel/drivers/nvme.h kernel/drivers/nvme.c
git commit -m "drivers: add NVMe controller driver (admin + I/O queues, blkdev registration)"
```

---

### Task 3: Wire nvme_init() into main.c and update Makefile

**Files:**
- Modify: `kernel/core/main.c`
- Modify: `Makefile`

- [ ] **Step 1: Add nvme.h include and nvme_init() call to main.c**

After the `pcie_init()` call in `kernel_main()`, add:

```c
#include "nvme.h"
```

And in the init sequence:

```c
    pcie_init();            /* enumerate PCIe devices — [PCIE] OK            */
    nvme_init();            /* NVMe block device — [NVME] OK or skip         */
```

- [ ] **Step 2: Add DRIVER_SRCS to Makefile**

Add a new source list and include it in the build:

```makefile
DRIVER_SRCS = \
    kernel/drivers/nvme.c
```

Add `DRIVER_OBJS` computation and include in the link step alongside existing ARCH_OBJS, FS_OBJS, etc.

Add `-Ikernel/drivers` to CFLAGS so `nvme.h` is found.

- [ ] **Step 3: Add blkdev.c to FS_SRCS**

In the `FS_SRCS` block, add `kernel/fs/blkdev.c`.

- [ ] **Step 4: Add `make disk` target**

```makefile
disk:
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(BUILD)/disk.img bs=1M count=64
	mke2fs -t ext2 -L aegis-root $(BUILD)/disk.img
```

- [ ] **Step 5: Update `make run` with NVMe + q35 flags**

The `run` target should use `-machine q35` and add NVMe disk attachment. Add a conditional: if `build/disk.img` exists, add NVMe flags:

```makefile
NVME_FLAGS = $(if $(wildcard $(BUILD)/disk.img),-drive file=$(BUILD)/disk.img,if=none,id=nvme0 -device nvme,drive=nvme0,serial=aegis0)
```

- [ ] **Step 6: Build**

```bash
make 2>&1 | tail -20
```

Fix any warnings. The nvme_init() will call `pcie_find_device()` — on `-machine pc` it returns NULL and nvme_init prints a skip message.

- [ ] **Step 7: Run make test (should still pass — nvme_init skips on -machine pc)**

```bash
make test 2>&1 | tail -20
```

NVMe does NOT add a boot.txt line — on `-machine pc` there is no NVMe device, and the driver silently returns. `make test` should remain GREEN.

- [ ] **Step 8: Commit**

```bash
git add kernel/core/main.c Makefile
git commit -m "phase20: wire NVMe driver into main.c, add DRIVER_SRCS and make disk target"
```

---

### Task 4: Write test_nvme.py and wire into run_tests.sh

**Files:**
- Create: `tests/test_nvme.py`
- Modify: `tests/run_tests.sh`

- [ ] **Step 1: Create test_nvme.py**

This test boots with `-machine q35` and an NVMe disk attached. It verifies `[NVME] OK:` appears in serial output. Follow the same pattern as `test_pipe.py`:
- Create a 64MB disk image with `dd` + `mke2fs`
- Boot QEMU with q35 + NVMe device + cdrom
- Read serial output
- Assert `[NVME] OK:` line is present

```python
#!/usr/bin/env python3
"""test_nvme.py — verify NVMe driver initializes on q35 with NVMe disk."""

import subprocess, sys, os, tempfile, time, signal

BOOT_TIMEOUT = 120
BUILD = os.path.join(os.path.dirname(__file__), '..', 'build')
ISO = os.path.join(BUILD, 'aegis.iso')

def create_disk(path):
    subprocess.run(['dd', 'if=/dev/zero', f'of={path}', 'bs=1M', 'count=64'],
                   check=True, capture_output=True)
    subprocess.run(['mke2fs', '-t', 'ext2', '-F', path],
                   check=True, capture_output=True)

def main():
    if not os.path.exists(ISO):
        print("FAIL: build/aegis.iso not found — run make iso first")
        sys.exit(1)

    with tempfile.NamedTemporaryFile(suffix='.img', delete=False) as f:
        disk_path = f.name

    try:
        create_disk(disk_path)

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

        nvme_ok = any('[NVME] OK:' in l for l in lines)
        if nvme_ok:
            print("PASS: NVMe driver initialized")
            sys.exit(0)
        else:
            print("FAIL: [NVME] OK: not found in serial output")
            print("--- serial output (kernel lines) ---")
            for l in lines:
                print(l)
            sys.exit(1)
    finally:
        os.unlink(disk_path)

if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Make test executable**

```bash
chmod +x tests/test_nvme.py
```

- [ ] **Step 3: Wire into run_tests.sh**

Add after existing test entries:

```bash
echo "--- test_nvme ---"
python3 tests/test_nvme.py || FAIL=1
```

- [ ] **Step 4: Run the test**

```bash
make INIT=shell iso && python3 tests/test_nvme.py
```

- [ ] **Step 5: Commit**

```bash
git add tests/test_nvme.py tests/run_tests.sh
git commit -m "tests: add NVMe driver smoke test (test_nvme.py)"
```

---

### Task 5: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 20 to Build Status table**

```markdown
| NVMe driver + blkdev (Phase 20) | ✅ Done | nvme_init on q35; blkdev_register; make test GREEN; test_nvme.py PASS |
```

- [ ] **Step 2: Add directory layout update**

Add `kernel/drivers/` to the directory layout section:

```
kernel/drivers/         ← Hardware drivers (protocol-agnostic, PCIe BAR access)
```

- [ ] **Step 3: Add Phase 20 forward-looking constraints**

```markdown
### Phase 20 forward-looking constraints

**NVMe I/O is synchronous doorbell+poll.** No interrupt-driven completion. Adequate for v1.0 shell workloads; MSI/MSI-X is v2.0 work.

**Queue memory is never freed.** Admin and I/O queue pages allocated via pmm_alloc_page are permanent. NVMe hot-remove is not supported.

**Single namespace only.** NSID=1 is hardcoded. Multi-namespace NVMe devices are not enumerated.

**No partition table parsing.** `nvme_init` registers `"nvme0"` as a whole-disk blkdev. GPT partition parsing (nvme0p1, nvme0p2) is Phase 25 work.

**BAR0 mapping assumes < 16 pages.** The NVMe register space is mapped via kva_alloc_pages. If a controller has a BAR0 larger than 64KB, the mapping is truncated.

**New build dependency: `e2fsprogs`.** `mke2fs` is used by `make disk`. Usually pre-installed on Debian/Ubuntu.
```

- [ ] **Step 4: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md build status for Phase 20"
```

---

## Final Verification

```bash
make test 2>&1 | tail -5
```

Expected: exit 0 (boot.txt unchanged — NVMe adds no line on `-machine pc`).

```bash
python3 tests/test_nvme.py
```

Expected: `PASS: NVMe driver initialized` (on q35 with NVMe disk).
