# Aegis v1.0 — Driver Stack + Installable Boot Design

> **For agentic workers:** implement this spec via `superpowers:subagent-driven-development` or `superpowers:executing-plans`. Each phase has a dedicated plan document.

**Goal:** Extend Aegis from a QEMU-only demo into a fully installable OS that boots on modern Ryzen 5000-7000 hardware with NVMe storage, USB HID keyboard, and AMD iGPU framebuffer display.

**Target hardware:** AMD Ryzen 5000-7000 (Zen 3/4), AMD iGPU (DCN 2.1 / 3.0 / 3.1), NVMe M.2 primary storage, USB keyboard (no PS/2 assumed).

**Strategy:** Approach A with QEMU-first discipline. Every driver is verified working in QEMU before any real-hardware testing. QEMU emulation exists for all subsystems except AMD DC — the framebuffer abstraction layer makes AMD DC testable in QEMU via the VESA backend.

**Bootloader:** GRUB EFI for v1.0. Custom EFI stub deferred to v2.0.

---

## Architecture

Everything hangs off PCIe as the common bus fabric. One enumeration loop discovers NVMe, xHCI, and AMD GPU. Three clean interfaces cross subsystem boundaries:

```
┌──────────────────────────────────────────────────┐
│                   User Space                      │
│           shell + wc + grep + sort + ...          │
├──────────────────────────────────────────────────┤
│                    VFS Layer                      │
│         ext2  │  console  │  kbd  │  pipe         │
├───────────────┬──────────────────────────────────┤
│   blkdev.h    │              fb.h                 │
│  (read/write  │     (framebuffer abstraction)     │
│   blocks)     │   VESA backend │ AMD DC backend   │
├───────┬───────┴──────┬───────────────────────────┤
│ NVMe  │    xHCI      │        PCIe               │
│driver │  USB stack   │   enumerate + config space │
│       │  HID→kbd VFS │                            │
├───────┴──────────────┴───────────────────────────┤
│                  ACPI tables                      │
│    (device discovery, interrupt routing)          │
└──────────────────────────────────────────────────┘
```

### Cross-boundary interfaces

**`kernel/fs/blkdev.h`**
```c
typedef struct blkdev {
    char     name[16];
    uint64_t block_count;
    uint32_t block_size;       /* 512 or 4096 */
    int (*read) (struct blkdev *, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *, uint64_t lba, uint32_t count, const void *buf);
} blkdev_t;

void     blkdev_register(blkdev_t *dev);
blkdev_t *blkdev_get(const char *name);  /* "nvme0" etc. */
```

NVMe registers one instance. ext2 holds a pointer. Future drivers (AHCI, virtio-blk) slot in without touching ext2.

**`kernel/drivers/`** — hardware drivers whose protocol is architecture-agnostic but whose implementation accesses PCIe BARs directly. Contains `nvme.c`/`nvme.h` (NVMe block driver), `xhci.c`/`xhci.h` (USB host controller), `usb_hid.c`/`usb_hid.h` (HID keyboard layer). These drivers call `pcie_read32`/`pcie_write32` via `kernel/arch/x86_64/pcie.h` — they depend on arch but are not themselves arch-specific in protocol. This directory must be added to the CLAUDE.md directory layout when Phase 20 begins.

**`kernel/fb/`** — framebuffer abstraction layer (architecture-agnostic). Contains `fb.c`, `fb.h`, `fb_vesa.c` (GRUB/GOP backend), and `fb_font.c` (glyph renderer). The AMD DC backend (`fb_amddc.c`) lives in `kernel/arch/x86_64/` because it contains x86-specific MMIO register access and PCI probing — consistent with the project rule that x86-specific code stays in `kernel/arch/x86_64/`. `fb_amddc.c` implements the same `fb_backend_t` interface as `fb_vesa.c` and registers itself via `fb_register_backend()`.

**`kernel/fb/fb.h`**
```c
typedef struct {
    uint32_t *addr;     /* mapped framebuffer base */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;    /* bytes per row */
    uint8_t   bpp;      /* 32 in practice */
} fb_info_t;

void       fb_init(void);
fb_info_t *fb_get_info(void);
void       fb_fill_rect(uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint32_t colour);
void       fb_blit_glyph(uint32_t x, uint32_t y,
                         uint8_t ch, uint32_t fg, uint32_t bg);
```

`fb_init()` probes AMD DC first (real hardware), falls back to VESA/GOP (QEMU and any GRUB-booted machine). printk and the shell renderer call only these functions — no backend-specific code in output paths.

**`kernel/arch/x86_64/pcie.h`**
```c
uint8_t  pcie_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);
uint16_t pcie_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);
uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);
void     pcie_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                      uint16_t off, uint32_t val);

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class;
    uint8_t  subclass;
    uint8_t  progif;
    uint8_t  bus, dev, fn;
    uint64_t bar[6];
} pcie_device_t;

int pcie_enumerate(pcie_device_t *out, int max_devices);
```

---

## Phase breakdown

### Phase 19: PCIe enumeration + ACPI MCFG/MADT

**Files:**
- Create: `kernel/arch/x86_64/pcie.c` + `pcie.h`
- Create: `kernel/arch/x86_64/acpi.c` + `acpi.h`
- Modify: `kernel/core/main.c` (wire init)
- Modify: `Makefile`

**ACPI scope (minimal):**
- Locate RSDP via multiboot2 ACPI tags — do NOT ROM-scan `[0xE0000, 0xFFFFF]`. Check tag type 15 (`MULTIBOOT2_TAG_TYPE_ACPI_NEW`, ACPI 2.0+ RSDP with XSDT) first; if absent, fall back to tag type 14 (`MULTIBOOT2_TAG_TYPE_ACPI_OLD`, ACPI 1.0 RSDP). GRUB on legacy BIOS (used by `make test` and `make run` with SeaBIOS/q35) typically provides type 14 only; GRUB on UEFI (used by `make run-disk` with OVMF) provides type 15. Both tags contain a physical pointer to the RSDP. Extend `arch_mm_init()` to save the RSDP physical address when parsing multiboot2 tags.
- Parse RSDP: if ACPI 2.0+ (RSDP revision ≥ 2, obtained from tag type 15 or type 14 with valid `Length` field), use `XsdtAddress` (64-bit) → XSDT with 64-bit table pointers. If ACPI 1.0 (revision 0, tag type 14), use `RsdtAddress` (32-bit) → RSDT with 32-bit table pointers. `-machine pc` (used by `make test`) provides a v1 RSDP; failing to handle RSDT will crash or miss all tables on this path.
- Find MCFG table → PCIe MMIO base address and bus range
- Find MADT table → APIC information (interrupt routing, for future MSI)
- No AML interpreter. No power management. Static table parsing only.

**PCIe enumeration:**
- Map MCFG MMIO window into kernel VA (each bus×device×function = 4KB config space)
- Scan all bus/device/function combinations
- Read vendor ID (skip if `0xFFFF`), class, subclass, progif, BARs
- **64-bit BAR decoding:** if BAR[n] bits `[2:1] == 0b10`, the address is 64-bit: combine `(BAR[n] & ~0xF) | ((uint64_t)BAR[n+1] << 32)` and skip BAR[n+1] in the iteration. NVMe BAR0 and xHCI BAR0 are typically 64-bit.
- Build a `pcie_device_t` table of discovered devices
- Print discovered devices to serial: `[PCIE] found 0x1002:0x1234 class=03 at 00:02.0`

**Machine type transition:** `make test` (serial oracle) stays on `-machine pc` — it does not test PCIe and must remain fast and deterministic. `make run` switches to `-machine q35` starting Phase 19, which exposes a PCIe root complex with ECAM config space. QEMU's q35 machine provides the same PCIe MMIO layout as real hardware. The two QEMU invocations intentionally diverge at this phase.

**QEMU test:** `make test` oracle stays on `-machine pc`. On `-machine pc`, QEMU's ACPI tables include MADT but no MCFG (legacy PCI uses IO-port config space, not ECAM). The ACPI init code must handle missing MCFG gracefully: find MADT, log `[ACPI] OK: MADT parsed, no MCFG (legacy machine)`, then log `[PCIE] OK: skipped (no ECAM)`. New `make run` uses `-machine q35`, which has full ECAM and prints `[PCIE] OK: enumeration complete, N devices`.

**Boot oracle change** (`tests/expected/boot.txt`): add exactly these two lines (matching the graceful-fallback messages for `-machine pc`):
```
[ACPI] OK: MADT parsed, no MCFG (legacy machine)
[PCIE] OK: skipped (no ECAM)
```

---

### Phase 20: NVMe driver + blkdev interface + QEMU disk

**Files:**
- Create: `kernel/drivers/nvme.c` + `nvme.h`
- Create: `kernel/fs/blkdev.c` + `blkdev.h`
- Modify: `Makefile` (add `-drive` + `-device nvme` to QEMU run targets)
- Create: `make disk` target producing `build/disk.img` (blank ext2, 64MB)

**NVMe init sequence:**
1. Find PCIe device class `0x01` / subclass `0x08` / progif `0x02`
2. Map BAR0 (controller registers, MMIO)
3. Disable controller: `CC.EN=0`, wait `CSTS.RDY=0`
4. Allocate Admin SQ (64 entries × 64 bytes = 4096 bytes) + Admin CQ (64 entries × 16 bytes = 1024 bytes) — NVMe SQ entries are 64 bytes; CQ entries are 16 bytes. Physical addresses written to `ASQ`/`ACQ` registers
5. Set `AQA` (admin queue sizes), set `CC.EN=1`, wait `CSTS.RDY=1`
6. Issue `Identify Controller` admin command — get model, firmware, max transfer size
7. Issue `Identify Namespace` (NSID=1) — get LBA format, namespace size in blocks
8. Create I/O SQ + I/O CQ (256 entries each) via `Create I/O Completion Queue` + `Create I/O Submission Queue` admin commands
9. Register `blkdev_t` as `nvme0`

**Read/write path:** submit NVMe Read/Write commands to I/O SQ, ring doorbell, poll CQ for completion. No interrupts in v1.0 — doorbell + poll is sufficient.

**Memory ordering requirements:**
- Before writing any SQ tail doorbell: issue `sfence` (store fence) to ensure all SQ entry writes are visible to the device before the doorbell write.
- When polling CQ entries: access the CQE Phase Tag and Status fields via `volatile` reads (or an explicit `lfence` after each read). Without `volatile`, the compiler may cache the first read of the Phase Tag and never see the device's update. Example: `volatile nvme_cqe_t *cqe = &cq[cq_head]; while ((cqe->status & 1) != phase) {} /* device has posted entry */`.

**QEMU device flags** (added to `make run` and Python tests):
```
-drive file=build/disk.img,if=none,id=nvme0
-device nvme,drive=nvme0,serial=aegis0
```

**Reference:** `references/nvme/pci.c`, `references/nvme/nvme.h`

---

### Phase 21: ext2 read-write filesystem

**Files:**
- Create: `kernel/fs/ext2.c` + `ext2.h`
- Modify: `kernel/fs/vfs.c` (register ext2 mount point)
- Modify: `kernel/core/main.c` (mount ext2 root at boot)
- Modify: `Makefile` (`make disk` populates /bin, /etc/motd)
- Create: `tests/test_ext2.py`
- Add shell commands: `mkdir`, `touch`, `rm`, `cp`, `mv`

**On-disk structures (ext2 spec):**
- Superblock at byte 1024: magic `0xEF53`, block size, inode count, block count
- Block group descriptor table follows superblock
- Each block group: block bitmap + inode bitmap + inode table + data blocks
- Inodes: 128 bytes, 12 direct + 1 indirect + 1 double-indirect block pointers
- Directory entries: 4-byte inode + 2-byte rec_len + 1-byte name_len + name

**Read path:** `open(path)` → walk directory entries from root inode (inode 2) → resolve each component → read inode → read data blocks via `blkdev_read`

**Write path:**
1. Allocate inode: scan inode bitmap for free bit, set it, write inode table entry
2. Allocate data blocks: scan block bitmap for free bits, set them
3. Write directory entry in parent directory (extend last block or allocate new)
4. Write inode (block pointers, size, timestamps)
5. Update superblock free counts + block group descriptor free counts

**No journal.** ext2 proper (not ext3/4). Dirty unmount is safe — `fsck.ext2` recovers. Same approach Linux used pre-ext3.

**Root device discovery (two-phase):**

- **Phase 21** mounts `nvme0` directly as a raw ext2 device (the Phase 21 `make disk` creates a raw ext2 image with no partition table; the superblock is at byte offset 1024 from LBA 0). The kernel calls `blkdev_get("nvme0")` and mounts ext2 on it. No GPT parsing in Phase 21.

- **Phase 25** introduces GPT parsing: after NVMe init, read LBA 1 (GPT header), walk the partition table, and register each partition as `nvme0p1`, `nvme0p2`, etc. with the appropriate LBA offset baked into the `blkdev_t` read/write callbacks. The root mount switches from `blkdev_get("nvme0")` to `blkdev_get("nvme0p2")` at this point. These two phases use the same ext2 code — only the `blkdev_t` pointer changes.

**Block cache:** a fixed 16-slot fully-associative cache with LRU eviction. Allocated from `kva_alloc_pages(16)` at ext2 mount time. Each cache slot holds one 4KB block (or one 1KB ext2 block padded to 4KB) plus a `block_num` tag and `dirty` flag. LRU eviction: a 16-entry age counter array; on each hit, increment the accessed slot's counter; on eviction, pick the slot with the lowest counter and write it back if dirty. This is adequate for v1.0 workloads (shell, small files); a full page cache is v2.0 work.

**`make disk` (Phase 21 — raw ext2, no partition table):**

Phase 21 introduces `make disk` as a raw ext2 image (no GPT, no EFI partition) for NVMe driver testing. This is simpler to create and verify before tackling the full partitioned image in Phase 25:
```makefile
disk:
    dd if=/dev/zero of=build/disk.img bs=1M count=64
    mke2fs -t ext2 -L aegis-root build/disk.img
    e2mkdir build/disk.img:/bin
    e2cp build/shell.elf build/disk.img:/bin/sh
    # ... other binaries
```

QEMU attaches `disk.img` directly to the NVMe device. The ext2 driver reads block 0 offset 1024 to find the superblock — this works on a raw ext2 image because there is no partition table; the filesystem starts at LBA 0. **Phase 25 replaces this target** with the full GPT version (`tools/mkdisk.sh`) that creates `build/aegis.img` instead.

New build dependency for `e2mkdir`/`e2cp`: `e2tools` package.

**Integration test `test_ext2.py`:** boots shell ISO + NVMe disk, creates `/tmp/hello.txt`, writes "hello", reboots, reads it back and verifies content persists.

---

### Phase 22: xHCI + USB HID keyboard

**Files:**
- Create: `kernel/drivers/xhci.c` + `xhci.h`
- Create: `kernel/drivers/usb_hid.c` + `usb_hid.h`
- Modify: `kernel/arch/x86_64/kbd.c` (probe USB HID as fallback/primary)
- Modify: `Makefile` (add `-device qemu-xhci -device usb-kbd` to QEMU run targets)

**xHCI init sequence:**
1. Find PCIe device class `0x0C` / subclass `0x03` / progif `0x30`
2. Map BAR0 (capability registers at base, operational registers at base+`CAPLENGTH`)
3. Stop: `USBCMD.RS=0`. Wait for `CSTS.HCH=1` (Halted) before proceeding — do NOT issue reset while the controller is still running. Then reset: `USBCMD.HCRST=1`. Wait for `USBCMD.HCRST=0` to clear (reset complete).
4. Allocate Device Context Base Address Array (DCBAA): `MaxSlots+1` pointers, write to `DCBAAP`
5. Allocate Command Ring (64-entry TRB ring), write to `CRCR`
6. Allocate Event Ring Segment (64 TRBs, 16-byte each = 1024 bytes) + Event Ring Segment Table (1 entry). The ERST entry's `RingSegmentSize` field must be set to `64` (number of TRBs, not bytes). Write `ERSTSZ = 1` (number of ERST entries) first — the controller must know the table size before reading any entries. Then write `ERSTBA` (ERST physical address) and `ERDP` (event ring dequeue pointer = segment base).
7. Set `USBCMD.RS=1`
8. Check each `PORTSC` register: if `CCS=1` (device connected), issue `Port Reset`

**Device enumeration (per port):**
1. `Enable Slot` command → slot ID
2. `Address Device` command with Input Context (endpoint 0 configured as control pipe)
3. `Get Descriptor` (Device Descriptor) — check class/subclass/protocol
4. `Get Descriptor` (Configuration Descriptor) — find interrupt IN endpoint
5. `Set Configuration`
6. If HID boot-class keyboard: `Set Protocol` (boot protocol), schedule interrupt IN transfers

**HID boot protocol:** 8-byte reports — `[modifier, reserved, key1..key6]`. Modifier byte bits: Left Ctrl (0), Left Shift (1), Left Alt (2), Left GUI (3), Right Ctrl (4), Right Shift (5), Right Alt (6), Right GUI (7). Key bytes are HID Usage IDs — look-up table maps to ASCII.

**Interrupt strategy:** poll event ring from PIT handler (100Hz). Sufficient for keyboard, avoids MSI complexity. USB HID reports arrive on the interrupt IN endpoint — we check the event ring for Transfer Event TRBs each PIT tick.

**Probe order in kbd.c:** PS/2 probe first (for QEMU compatibility), xHCI/USB HID second. Both can coexist. On Ryzen hardware without PS/2, only USB HID responds.

**QEMU flags added:**
```
-device qemu-xhci -device usb-kbd
```
QEMU's `sendkey` monitor command routes to whichever keyboard device is active — existing test scripts require no changes.

**Reference:** `references/xhci/xhci.h`, `references/xhci/xhci-ring.c`, `references/xhci/hid.h`

---

### Phase 23: Framebuffer abstraction + VESA/GOP backend

**Files:**
- Create: `kernel/fb/fb.c` + `fb.h`
- Create: `kernel/fb/fb_vesa.c` (VESA/GOP backend)
- Create: `kernel/fb/fb_font.c` (8×16 glyph renderer from VGA ROM font)
- Modify: `kernel/core/printk.c` (add framebuffer output path)
- Modify: `kernel/core/main.c` (call `fb_init()`)
- Modify: `Makefile`

**VESA/GOP backend:** GRUB passes a multiboot2 framebuffer tag (type 8) containing `framebuffer_addr` (physical), `framebuffer_pitch`, `framebuffer_width`, `framebuffer_height`, `framebuffer_bpp`. `arch_mm_init()` must be extended to scan multiboot2 tags for type `MULTIBOOT_TAG_TYPE_FRAMEBUFFER` (value 8) and save the physical address and dimensions into a global `boot_fb_t` struct before the VMM is up. Then `fb_vesa_init()` (called after VMM) maps `framebuffer_addr` into kernel VA via `kva_alloc_pages` + `vmm_map_page` and fills in `fb_info_t`. This works in QEMU day one with no additional flags — GRUB sets up the framebuffer via BIOS/UEFI before handing off to the kernel.

**Text renderer:** `fb_font.c` embeds a static 8×16 glyph table as a `uint8_t font_data[256][16]` array compiled directly into the kernel (the standard PC font, ~4KB). Do NOT read VGA ROM at `0xC0000` — that address is only mapped in real/protected mode and is inaccessible in long mode without a special MMIO mapping. The static array is the correct approach. `fb_blit_glyph` draws one character as 128 pixels (8 wide × 16 tall) in the requested fg/bg colours.

**printk update:** gains a framebuffer path alongside existing serial and VGA text paths. Serial always comes first (CLAUDE.md Law 2 — if framebuffer faults, the character is already on serial):
```c
serial_putchar(c);           /* ALWAYS FIRST — serial must work even if display fails */
if (fb_get_info() != NULL)
    fb_console_putchar(c);   /* scrolling framebuffer text console */
else
    vga_putchar(c);          /* text-mode fallback when no framebuffer */
```

Scrolling: when cursor reaches the last row, `memmove` the framebuffer up by one glyph row (16 × pitch bytes), clear the last row, decrement row counter.

**`make test` oracle behavior:** `fb_init()` must print a serial status line in both cases so `boot.txt` has a deterministic entry:
- If a multiboot2 framebuffer tag (type 8) is present: `[FB] OK: %ux%u bpp=%u addr=0x%lx`
- If no framebuffer tag: `[FB] OK: no framebuffer tag (text-mode only)`

On `-machine pc`, GRUB only passes a framebuffer tag if the kernel's multiboot2 header includes a framebuffer request (`MULTIBOOT_HEADER_TAG_FRAMEBUFFER`). Phase 23 must add this tag to the kernel's multiboot2 header. Without it, GRUB will not set up a linear framebuffer and the VESA backend falls back to no-op. Both paths print an `[FB] OK:` line so `boot.txt` always contains exactly one `[FB]` line. Update `tests/expected/boot.txt` with the appropriate message for `-machine pc`.

Add `test_fb.py` that boots and verifies the `[FB] OK:` line appears in serial output.

---

### Phase 24: AMD DC framebuffer backend (Ryzen 5000-7000)

**Files:**
- Create: `kernel/arch/x86_64/fb_amddc.c` + `kernel/arch/x86_64/fb_amddc.h` (x86-specific, MMIO/PCI access)
- Create: `kernel/arch/x86_64/i2c_ddc.c` (DDC/I2C for EDID reading)
- Modify: `kernel/fb/fb.c` (probe AMD DC before VESA)

**Scope:** single display output at native resolution, linear framebuffer in VRAM. No 3D, no multi-display, no power management, no HDMI audio.

**DCN version detection:** read the `IP_VERSION` register at BAR0 offset `0x2600C` after mapping BAR0. This register is available on all GFX9+ AMD GPUs and encodes the DCN major.minor version directly, avoiding a fragile device-ID lookup table. Decode as:
```c
uint32_t ip_ver = mmio_read32(bar0 + 0x2600C);
uint32_t major  = (ip_ver >> 16) & 0xFF;  /* e.g. 2 for DCN2.x */
uint32_t minor  = (ip_ver >>  8) & 0xFF;  /* e.g. 1 for DCN2.1 */
```
Supported: DCN 2.1 (Cezanne / Ryzen 5000G), DCN 3.0 (Rembrandt / Ryzen 6000), DCN 3.1 (Phoenix / Ryzen 7040). Reject any other major version with `[FB] WARN: AMD DC unsupported DCN version, falling back to VESA`.

Each version has its own register offset table (sourced from `references/amd-dc/dcn21_resource.c` etc.) but the same init sequence structure.

**Init sequence:**
1. PCIe: find AMD GPU (vendor `0x1002`, class `0x03`), map BAR0 (register space) and BAR5 (VRAM aperture, typically 256MB–2GB)
2. Read golden register values for DCN version (from resource file tables)
3. Minimal clock init: verify display clock (DCLK) is running — skip frequency adjustment, just check
4. Read EDID via DDC I2C (I2C master built into AMD DC register block): get native resolution and timings. **Must happen before CRTC programming** — the EDID timings are the input to step 8. Fall back to 1920×1080@60Hz if DDC read fails.
5. Allocate scanout framebuffer: carve a region from VRAM aperture aligned to 256-byte boundary, sized for `width × height × 4` bytes
6. Configure HUBP (Hub and Prefetch): set surface address (VRAM physical), pitch, pixel format (ARGB8888)
7. Configure DPP (Display Pipe and Plane): identity color matrix, no scaling
8. Configure OPP (Output Pixel Processing): bypass LUT
9. Configure OPTC (Output Pipe and Timing Controller): program CRTC timings using values from EDID (step 4)
10. Configure DIO (Display I/O): enable encoder for the physical connector (DisplayPort or HDMI)
11. Register `fb_info_t` with physical VRAM framebuffer address and native resolution

**EDID fallback:** if DDC read fails, assume 1920×1080@60Hz. Covers most common cases.

**Testing:** real Ryzen hardware only. QEMU uses VESA backend. Integration test: boot on hardware, verify serial output includes `[FB] OK: AMD DC DCN2.1 1920x1080` (or similar).

**Reference:** `references/amd-dc/dc.h`, `references/amd-dc/dcn21_resource.c`, `references/amd-dc/dcn30_resource.c`, `references/amd-dc/dcn31_resource.c`

---

### Phase 25: Installable disk image

**Files:**
- Modify: `Makefile` (add `disk`, `run-disk`, `install` targets)
- Create: `tools/mkdisk.sh` (disk image creation script)
- Modify: QEMU invocation in run targets and Python tests

**Disk layout (GPT):**
```
Partition 1: EFI System Partition, 32MB, FAT32
    /EFI/BOOT/BOOTX64.EFI   ← GRUB EFI binary (--removable path)
    /boot/grub/grub.cfg
    /boot/aegis.elf          ← kernel
    /boot/.disk-uuid          ← UUID of this ESP (written by mkdisk.sh for grub.cfg)

Partition 2: ext2 root, remainder (default 256MB total image)
    /bin/   sh ls cat echo pwd uname clear true false wc grep sort
    /etc/   motd
    /tmp/
    /home/
```

**`make disk` sequence (`tools/mkdisk.sh`):**

Note: this script requires root (`sudo make disk`) because it uses `losetup`, `mount`, `grub-install`, and `mkfs.*`. A future phase may replace these with rootless `guestfish` equivalents.

```bash
dd if=/dev/zero of=build/aegis.img bs=1M count=256
sgdisk -n 1:2048:+32M -t 1:ef00 -n 2:0:0 -t 2:8300 build/aegis.img

# Attach loop device, format and populate ESP
LOOP=$(losetup -fP --show build/aegis.img)
mkfs.fat -F32 ${LOOP}p1
mkfs.ext2 -L aegis-root ${LOOP}p2

# Mount ESP, install GRUB EFI bootloader
mount ${LOOP}p1 /mnt/esp
grub-install --target=x86_64-efi \
             --efi-directory=/mnt/esp \
             --boot-directory=/mnt/esp/boot \
             --removable \
             --no-nvram

# Write grub.cfg
mkdir -p /mnt/esp/boot/grub
# Get ESP UUID for use in grub.cfg (avoids fragile hd0 assumption)
ESP_UUID=$(blkid -s UUID -o value ${LOOP}p1)
cat > /mnt/esp/boot/grub/grub.cfg << EOF
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
EOF

# Copy kernel into ESP /boot/
cp build/aegis.elf /mnt/esp/boot/aegis.elf
umount /mnt/esp

# Populate ext2 root partition
mount ${LOOP}p2 /mnt/root
mkdir -p /mnt/root/{bin,etc,tmp,home}
# copy userspace binaries
cp build/shell.elf   /mnt/root/bin/sh
cp build/ls.elf      /mnt/root/bin/ls
# ... other binaries
echo "Welcome to Aegis" > /mnt/root/etc/motd
umount /mnt/root
losetup -d ${LOOP}
```

**New build dependencies for Phase 25** (added to CLAUDE.md toolchain table):
- `grub-efi-amd64-bin` — provides `grub-install --target=x86_64-efi`
- `mtools` — FAT manipulation (required by some grub-install variants)
- `ovmf` — UEFI firmware for QEMU (`OVMF_CODE_4M.fd`, `OVMF_VARS_4M.fd`)
- `gdisk` — provides `sgdisk` for GPT partition creation
- `util-linux` — provides `blkid` for UUID extraction in `mkdisk.sh` (usually pre-installed)

Note: `--removable` writes to `/EFI/BOOT/BOOTX64.EFI` (the fallback path), avoiding the need to write to NVRAM. This is correct for a disk image that will be installed to multiple machines.

**`make install`:**
```bash
@echo "WARNING: this will ERASE the target disk."
@read -p "Type the full device path (e.g. /dev/sdb) to confirm: " DISK; \
    dd if=build/aegis.img of=$$DISK bs=4M status=progress
```
No `-y` flag. Requires explicit device path re-entry. No automation bypass.

**`make run-disk`:** boots `aegis.img` in QEMU via NVMe (replaces ISO boot for full-stack testing). Because `aegis.img` uses GPT + EFI System Partition + GRUB EFI, this target requires UEFI firmware (OVMF). SeaBIOS cannot execute `BOOTX64.EFI`:
```
-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
-drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd
-drive file=build/aegis.img,if=none,id=nvme0
-device nvme,drive=nvme0,serial=aegis0
-device qemu-xhci -device usb-kbd
```
`build/OVMF_VARS_4M.fd` is a writable copy of `OVMF_VARS_4M.fd` created by `make disk` from the system-provided file (in `/usr/share/OVMF/` on Debian). New build dependency: `ovmf` package.

**`make run`** (ISO boot, unchanged from Phase 16) uses SeaBIOS + multiboot2 ISO — this still works because GRUB-PC is on the ISO and speaks multiboot2 to our kernel. The ISO and disk-image paths are intentionally separate boot chains.

**Existing `make test`** (serial oracle) unchanged — still boots from ISO, diffs boot.txt.

---

### Phase 26: Documentation + v1.0 release

**Files:**
- Create: `README.md` — what Aegis is, quick start, hardware requirements, QEMU usage
- Create: `CONTRIBUTING.md` — architecture overview, coding standards, how to add a driver
- Create: `LICENSE` — MIT
- Create: `docs/architecture.md` — full subsystem reference (kernel map, design decisions, why each choice was made)
- Create: `docs/hardware-support.md` — supported/tested hardware list
- Modify: `CLAUDE.md` — final build status table

**v1.0 tag:** `git tag -s v1.0.0 -m "Aegis v1.0.0 — first public release"`

**GitHub publish:** public repo, MIT license, `README.md` as landing page.

---

## QEMU reference invocations

**`make test` (serial oracle — unchanged):**
```
qemu-system-x86_64 -machine pc -cpu Broadwell -cdrom build/aegis.iso
    -boot order=d -display none -vga std -nodefaults -serial stdio
    -no-reboot -m 128M -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

**`make run` (interactive, post-Phase 20):**
```
qemu-system-x86_64 -machine q35 -cpu Broadwell
    -drive file=build/disk.img,if=none,id=nvme0
    -device nvme,drive=nvme0,serial=aegis0
    -device qemu-xhci -device usb-kbd
    -cdrom build/aegis.iso -boot order=d
    -m 256M -serial stdio -vga std
```

**`make run-disk` (full disk boot, post-Phase 25) — requires UEFI firmware:**
```
qemu-system-x86_64 -machine q35 -cpu Broadwell
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
    -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd
    -drive file=build/aegis.img,if=none,id=nvme0
    -device nvme,drive=nvme0,serial=aegis0
    -device qemu-xhci -device usb-kbd
    -m 256M -serial stdio
    -display gtk
```
(The `-display gtk` flag makes the framebuffer window visible.)

---

## Forward-looking constraints (v2.0)

- **Custom EFI stub** replaces GRUB. Requires PE header, UEFI memory map parsing, GOP framebuffer setup without multiboot2.
- **MSI/MSI-X** interrupts for NVMe and xHCI. Requires IOAPIC driver and interrupt remapping.
- **AHCI fallback** for systems with SATA SSDs.
- **NVMe queues per CPU** for SMP.
- **AMD DC power management** — current init leaves clocks at firmware state.
- **Multi-display** — current AMD DC init assumes one output.
- **USB mass storage** — `usb-storage` class driver over xHCI, enabling USB boot and external drives.
- **Capability model** (v2.0 core feature) — fork from v1.0 codebase, redesign syscall layer around unforgeable capability tokens.
