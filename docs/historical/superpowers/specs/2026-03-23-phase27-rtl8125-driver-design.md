# Phase 27: RTL8125 2.5GbE Driver Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a kernel driver for the Realtek RTL8125B 2.5GbE NIC, registering it as a `netdev_t` so the Phase 25 protocol stack and Phase 26 socket API work on real hardware without modification.

**Architecture:** Registers as `"eth0"` (or `"eth1"` if virtio-net is also present). Uses MMIO descriptor ring model (16-byte descriptors, similar to NVMe SQ/CQ). Polled from PIT tick handler, same as virtio-net. No firmware blob required for RTL8125B rev 05.

**Tech Stack:** C, PCIe BAR2 MMIO (64K), KVA-mapped registers, PMM-allocated DMA rings.

---

## ⚠️ TESTING DEFERRED

**This driver is written and compiled in Phase 27 but its integration test (`test_rtl8125.py`) is excluded from `run_tests.sh` and is NOT run in CI.**

The host machine's RTL8125B (PCI `0a:00.0`, ASUS subsystem, IOMMU group 18) is currently managed by the Linux `r8169` driver. Testing the Aegis driver requires:
- **Option A:** VFIO passthrough — `modprobe vfio-pci`, bind `0a:00.0` to `vfio-pci`, pass `-device vfio-pci,host=0a:00.0` to QEMU
- **Option B:** Native boot — write Aegis to disk and boot bare-metal

**Do NOT test until the host WiFi (MediaTek MT7921K, `0b:00.0`) is confirmed working**, as rebinding the RTL8125 to VFIO will drop the Ethernet connection. If the session is remote, this will cut off access.

`make test` is unaffected: uses `-machine pc`, no RTL8125 present, `rtl8125_init()` returns silently.

---

## Hardware Details

| Property | Value |
|----------|-------|
| PCI Vendor | `0x10EC` (Realtek) |
| PCI Device | `0x8125` |
| Subsystem | ASUSTeK `0x87D7` |
| BAR0 | I/O ports, 256 bytes (do not use) |
| BAR2 | 64-bit MMIO, 64KB — primary register interface |
| BAR4 | 64-bit MMIO, 16KB — MSI-X table (not used in Phase 27) |
| Revision | 05 (RTL8125BG) |
| Linux driver | `r8169` |
| Speed | 2.5 Gbps / 1 Gbps / 100 Mbps auto-negotiate |
| PHY init | MDIO register writes (no binary firmware blob needed for rev 05) |

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `kernel/drivers/rtl8125.h` | Create | Register offsets, descriptor layout, init/poll/send API |
| `kernel/drivers/rtl8125.c` | Create | Full driver implementation |
| `kernel/core/main.c` | Modify | Call `rtl8125_init()` after `virtio_net_init()` |
| `Makefile` | Modify | Add `rtl8125.c` to DRV_SRCS |
| `tests/test_rtl8125.py` | Create | VFIO passthrough test (excluded from run_tests.sh) |

---

## Register Map (BAR2 MMIO offsets)

```c
#define RTL_IDR0        0x00   /* MAC address bytes 0–5 (read 6 bytes) */
#define RTL_MAR0        0x08   /* Multicast filter (8 bytes) */
#define RTL_DTCCR       0x10   /* Dump tally counter command */
#define RTL_TNPDS       0x20   /* TX normal priority descriptor start (64-bit phys) */
#define RTL_THPDS       0x28   /* TX high priority descriptor start (64-bit phys) */
#define RTL_CR          0x37   /* Command register */
#define RTL_TPPOLL      0x38   /* TX polling (write 0x40 to kick normal queue) */
#define RTL_IMR         0x3C   /* Interrupt mask register (16-bit) */
#define RTL_ISR         0x3E   /* Interrupt status register (16-bit) */
#define RTL_TCR         0x40   /* TX configuration register (32-bit) */
#define RTL_RCR         0x44   /* RX configuration register (32-bit) */
#define RTL_CFG9346     0x50   /* EEPROM control: 0xC0 = unlock, 0x00 = lock */
#define RTL_RDSAR       0xE4   /* RX descriptor start address (64-bit phys) */
#define RTL_MTPS        0xEC   /* Max TX packet size (8-bit, units of 128 bytes) */
#define RTL_ETThR       0xEC   /* TX threshold (same register, different name in some docs) */
#define RTL_RMS         0xDA   /* RX max packet size (16-bit) */
#define RTL_CPLUS_CMD   0xE0   /* C+ command register (16-bit) */
```

Key bits:
- `CR`: bit 4 = RST (write 1 to reset; poll until 0), bit 3 = TE (TX enable), bit 2 = RE (RX enable)
- `RCR`: `APM` (0x2) accept physical match, `AM` (0x4) accept multicast, `AB` (0x8) accept broadcast, `MXDMA` (0x700) max DMA burst = 1024 bytes (0x600)
- `TCR`: `MXDMA` (0x700) = 0x600 (1024 bytes), `IFG` (0x18000) = 0x18000 (96-bit IFG)

---

## Descriptor Format (16 bytes)

```c
typedef struct __attribute__((packed)) {
    uint32_t opts1;    /* OWN(31) | EOR(30) | FS(29) | LS(28) | len[12:0] */
    uint32_t opts2;    /* VLAN/checksum offload flags — set to 0 in Phase 27 */
    uint64_t addr;     /* physical address of packet buffer */
} rtl_desc_t;
```

- **OWN bit (bit 31):** set by driver to hand descriptor to NIC; NIC clears on completion
- **EOR bit (bit 30):** End Of Ring — set on last descriptor in ring to wrap around
- **FS/LS (bits 29/28):** First Segment / Last Segment — set both for single-segment frames
- **len[12:0]:** buffer length for TX (frame length); filled by NIC for RX (received frame length)

---

## Init Sequence

```c
void rtl8125_init(void) {
    /* 1. Find device: scan pcie_get_devices() for vendor=0x10EC, device=0x8125 */
    /* 2. If not found: return silently */

    /* 3. Map BAR2 (64K MMIO) via kva_alloc_pages + vmm_map_page, no-cache flags */

    /* 4. Soft reset — poll with timeout (max 10000 iterations) */
    mmio_write8(base + RTL_CR, 0x10);           /* RST = 1 */
    for (int i = 0; i < 10000; i++) {
        if (!(mmio_read8(base + RTL_CR) & 0x10)) break;
        if (i == 9999) {
            printk("[RTL8125] reset timeout — hardware fault\n");
            return;
        }
    }

    /* 5. Unlock config registers */
    mmio_write8(base + RTL_CFG9346, 0xC0);

    /* 6. Read MAC address from IDR0–IDR5 */
    for (int i = 0; i < 6; i++)
        mac[i] = mmio_read8(base + RTL_IDR0 + i);

    /* 7. Allocate RX descriptor ring (256 × 16 bytes = 4096 = 1 page) */
    /*    rx_ring_phys = pmm_alloc_page() — physical address returned by PMM */
    /*    rx_ring = kva-mapped virtual address of rx_ring_phys (for CPU access) */
    /*    Allocate 256 × 2KB RX packet buffers (one PMM page each, use first 2KB) */
    /*    rx_virt[i] = kva-mapped virtual addr; descriptor addr field = phys (PMM value) */
    /*    Fill descriptors: opts1 = OWN | 2048, addr = rx_buf_phys[i] (PMM physical) */
    /*    Set EOR on descriptor 255 */
    mmio_write64(base + RTL_RDSAR, rx_ring_phys); /* NIC needs physical address */

    /* 8. Allocate TX descriptor ring (256 × 16 bytes = 1 page) */
    /*    tx_ring_phys = pmm_alloc_page() — physical address for NIC MMIO register */
    /*    tx_ring = kva-mapped virtual address for CPU writes to descriptors */
    /*    All descriptors start with OWN=0, addr=0 */
    /*    Set EOR on descriptor 255 */
    mmio_write64(base + RTL_TNPDS, tx_ring_phys); /* NIC needs physical address */

    /* 9. Configure RX */
    mmio_write16(base + RTL_RMS, 2048);          /* max RX frame size */
    mmio_write32(base + RTL_RCR, 0x0000E70F);   /* APM|AM|AB|MXDMA=1024|no_wrap */

    /* 10. Configure TX */
    mmio_write32(base + RTL_TCR, 0x03000700);   /* MXDMA=1024, IFG=96bit */
    mmio_write8(base + RTL_MTPS, 0x3B);         /* max TX size = 0x3B × 128 = 7552 bytes */

    /* 11. Configure C+ command register (enable RX/TX checksumming — disabled) */
    mmio_write16(base + RTL_CPLUS_CMD, 0x2000); /* RX VLAN destrip off */

    /* 12. Mask all interrupts (poll-only) */
    mmio_write16(base + RTL_IMR, 0x0000);

    /* 13. Enable TX and RX */
    mmio_write8(base + RTL_CR, 0x0C);           /* TE | RE */

    /* 14. Lock config registers */
    mmio_write8(base + RTL_CFG9346, 0x00);

    /* 15. Register as netdev_t "eth0" */
    /* 16. printk("[NET] OK: rtl8125 eth0 mac=...") */
}
```

---

## TX Path

```c
int rtl8125_send(netdev_t *dev, const void *pkt, uint16_t len) {
    rtl_priv_t *p = dev->priv;
    uint8_t idx = p->tx_head & 255;
    /* copy pkt into pre-allocated TX buffer for this descriptor slot */
    memcpy(p->tx_virt[idx], pkt, len);
    /* build descriptor */
    p->tx_ring[idx].addr  = p->tx_phys[idx];
    p->tx_ring[idx].opts2 = 0;
    uint32_t opts1 = (1u<<31)|(1u<<29)|(1u<<28)|len; /* OWN|FS|LS|len */
    if (idx == 255) opts1 |= (1u<<30);               /* EOR */
    p->tx_ring[idx].opts1 = opts1;                   /* write OWN last */
    /* kick TX */
    mmio_write8(p->base + RTL_TPPOLL, 0x40);
    p->tx_head++;
    /* poll for completion (wait until OWN clears), with timeout */
    for (int i = 0; i < 100000; i++) {
        if (!(p->tx_ring[idx].opts1 & (1u<<31)))
            return 0;
    }
    printk("[RTL8125] TX timeout on descriptor %u — link down?\n", (unsigned)idx);
    return -1; /* -EIO */
}
```

---

## RX Poll

```c
void rtl8125_poll(netdev_t *dev) {
    rtl_priv_t *p = dev->priv;
    uint8_t idx = p->rx_tail & 255;
    while (!(p->rx_ring[idx].opts1 & (1u<<31))) { /* OWN=0 means NIC filled it */
        uint32_t opts1 = p->rx_ring[idx].opts1;
        uint16_t len = (opts1 & 0x1FFF) - 4;      /* strip 4-byte FCS */
        /* deliver to Ethernet layer */
        netdev_rx_deliver(dev, p->rx_virt[idx], len);
        /* return descriptor to NIC */
        uint32_t new_opts1 = (1u<<31) | 2048;      /* OWN=1, len=2048 */
        if (idx == 255) new_opts1 |= (1u<<30);     /* EOR */
        p->rx_ring[idx].opts1 = new_opts1;
        p->rx_tail++;
        idx = p->rx_tail & 255;
    }
}
```

---

## MMIO Access Helpers

Per architecture rules, no MMIO magic numbers outside `kernel/arch/x86_64/`. Provide arch-agnostic wrappers in `kernel/drivers/rtl8125.c` using `volatile` pointer dereferences (same pattern as existing ECAM access in `pcie.c`):

```c
static inline void mmio_write8 (uint64_t base, uint16_t off, uint8_t  v) { *(volatile uint8_t  *)(base+off) = v; }
static inline void mmio_write16(uint64_t base, uint16_t off, uint16_t v) { *(volatile uint16_t *)(base+off) = v; }
static inline void mmio_write32(uint64_t base, uint16_t off, uint32_t v) { *(volatile uint32_t *)(base+off) = v; }
/* mmio_write64: single 64-bit write. Valid on x86_64 accessing PCIe BAR2 MMIO.
 * Do NOT split into two 32-bit writes — the NIC latches TNPDS/RDSAR as a unit. */
static inline void mmio_write64(uint64_t base, uint16_t off, uint64_t v) { *(volatile uint64_t *)(base+off) = v; }
static inline uint8_t  mmio_read8 (uint64_t base, uint16_t off) { return *(volatile uint8_t  *)(base+off); }
static inline uint16_t mmio_read16(uint64_t base, uint16_t off) { return *(volatile uint16_t *)(base+off); }
static inline uint32_t mmio_read32(uint64_t base, uint16_t off) { return *(volatile uint32_t *)(base+off); }
```

---

## Testing

### `tests/test_rtl8125.py`

**Excluded from `run_tests.sh`.** Documents the VFIO passthrough procedure:

```python
# PREREQUISITE: WiFi (MT7921K, 0b:00.0) must be configured and verified working
# before running this test. Rebinding 0a:00.0 to vfio-pci will drop Ethernet.
#
# Setup:
#   sudo modprobe vfio vfio-pci
#   echo "10ec 8125" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id
#   echo "0000:0a:00.0" | sudo tee /sys/bus/pci/devices/0000:0a:00.0/driver/unbind
#   echo "0000:0a:00.0" | sudo tee /sys/bus/pci/drivers/vfio-pci/bind
#
# QEMU flags (append to normal q35 boot):
#   -device vfio-pci,host=0a:00.0
```

Test verifies `[NET] OK: rtl8125 eth0` in serial output and sends a ping.

---

## Forward-Looking Constraints

**Poll-only at 100Hz.** MSI-X interrupt-driven completion requires enabling BAR4 and programming the MSI-X table. Deferred to v2.0.

**TX serialized.** One frame in flight at a time (busy-poll for OWN clear). The TX ring has 256 slots but only slot 0 is used. Full ring pipelining requires tracking `tx_head` and `tx_tail` independently and not polling for completion synchronously.

**No jumbo frames.** RX buffers are 2KB. Jumbo frames (up to 9KB) require scatter-gather RX descriptors.

**No hardware checksum offload.** `opts2` is always 0. RTL8125 supports TX checksum offload via `opts2` bits — can be enabled later for TCP/UDP performance.

**No link status monitoring.** Link up/down events are not tracked. If the cable is unplugged, sends will silently fail (TX OWN never clears — the busy-poll will hang). Add link change interrupt handling before production use.
