# Phase 24: netdev_t Abstraction + virtio-net Driver Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Establish a `netdev_t` network device abstraction layer and implement a virtio-net driver against it, providing the hardware send/receive foundation for the protocol stack.

**Architecture:** Mirrors the `blkdev_t` / NVMe pattern — `netdev_t` is a registry of named network devices with send/poll callbacks; virtio-net registers as `"eth0"`. RX is polled from the PIT tick handler (same as USB HID). All DMA buffers use the existing KVA + PMM allocators.

**Tech Stack:** C, virtio 1.0 modern (MMIO) transport, PCI capability list walking, KVA-mapped BARs, PIT polling.

---

## Constraints and Non-Negotiables

- `make test` uses `-machine pc` — no virtio-net present. `virtio_net_init()` must return silently (no printk) if no device is found. Boot oracle unchanged.
- virtio-net uses **modern (MMIO) transport only** — no port I/O in `kernel/drivers/`. Port I/O is restricted to `kernel/arch/x86_64/` per architecture rules.
- All DMA buffers (descriptor tables, receive buffers) must be physically contiguous pages allocated via `pmm_alloc_page()` and mapped via KVA window. Physical addresses passed to NIC, virtual addresses used by CPU.
- `netdev_rx_deliver()` is called from the PIT tick ISR context — it must not block and must not call `sched_block()`.
- `NETDEV_MAX = 4`. Static table, no dynamic allocation.
- RX receive buffers: 1536 bytes each (1500 MTU + 14 Ethernet header + 10 virtio_net_hdr + 12 headroom). Pre-allocated at init, never freed.

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `kernel/net/netdev.h` | Create | `netdev_t` struct, registry API, `netdev_rx_deliver` |
| `kernel/net/netdev.c` | Create | Static registry, dispatch to upper layer (stub in Phase 24) |
| `kernel/drivers/virtio_net.h` | Create | virtio register layout, virtqueue structs, capability offsets |
| `kernel/drivers/virtio_net.c` | Create | Driver init, TX send, RX poll |
| `kernel/core/main.c` | Modify | Call `virtio_net_init()` after `nvme_init()` |
| `Makefile` | Modify | Add `kernel/net/netdev.c` to NET_SRCS, `kernel/drivers/virtio_net.c` to DRV_SRCS |
| `tests/expected/boot.txt` | Modify | Add `[NET] OK: virtio-net eth0 mac=...` — wait, boot oracle uses -machine pc (no NIC), so NO change |
| `tests/test_virtio_net.py` | Create | Boot q35 + virtio-net + SLIRP, verify `[NET] OK` in serial |
| `tests/run_tests.sh` | Modify | Add `test_virtio_net.py` |

---

## Interface Definitions

### `kernel/net/netdev.h`

```c
#ifndef NETDEV_H
#define NETDEV_H

#include <stdint.h>

#define NETDEV_MAX 4

typedef struct netdev {
    char     name[16];       /* "eth0", "eth1" */
    uint8_t  mac[6];         /* hardware MAC address */
    uint16_t mtu;            /* maximum transmission unit (1500) */
    /* send: transmit one Ethernet frame. Returns 0 on success, -1 on error.
     * Called from process context (syscall path). Must not block. */
    int    (*send)(struct netdev *dev, const void *pkt, uint16_t len);
    /* poll: check for received frames. Called from PIT tick ISR.
     * Must not block. Delivers received frames via netdev_rx_deliver(). */
    void   (*poll)(struct netdev *dev);
    void    *priv;           /* driver-private data */
} netdev_t;

/* Register a network device. Returns 0 on success, -1 if table full. */
int       netdev_register(netdev_t *dev);

/* Look up a network device by name. Returns NULL if not found. */
netdev_t *netdev_get(const char *name);

/* Called by drivers when a frame is received. Dispatches to upper layer.
 * frame points to raw Ethernet frame (including 14-byte header).
 * Safe to call from ISR context. */
void netdev_rx_deliver(netdev_t *dev, const void *frame, uint16_t len);

#endif /* NETDEV_H */
```

---

## virtio-net Driver Design

### PCI Discovery

```c
/* virtio-net: vendor 0x1AF4, device 0x1041 (modern) or 0x1000 (legacy transitional).
 * Use pcie_get_devices() loop and check vendor_id, not pcie_find_device(). */
for (int i = 0; i < pcie_device_count(); i++) {
    const pcie_device_t *d = &pcie_get_devices()[i];
    if (d->vendor_id == 0x1AF4 &&
        (d->device_id == 0x1041 || d->device_id == 0x1000)) {
        /* found */
    }
}
```

### PCI Capability Walking

virtio 1.0 modern devices advertise their register regions via PCI vendor-specific capabilities (type 0x09). Walk the capability list from config offset 0x34, find sub-types:
- `VIRTIO_PCI_CAP_COMMON_CFG` (1) — device-wide config
- `VIRTIO_PCI_CAP_NOTIFY_CFG` (2) — queue kick doorbell
- `VIRTIO_PCI_CAP_DEVICE_CFG` (4) — device-specific config (MAC, status)

Each capability encodes a BAR index + offset + length. Map the BAR via `kva_alloc_pages()` + `vmm_map_page()` with no-cache flags `0x1Bu` (Present|Write|PWT|PCD), matching the established driver pattern from `nvme.c`/`xhci.c`.

### Virtqueue Layout (per queue)

Each virtqueue has three components, each allocated as a separate PMM page:
- **Descriptor table:** 16 bytes × 256 entries = 4096 bytes (1 page). Fields: `addr` (physical), `len`, `flags` (NEXT, WRITE), `next`.
- **Available ring:** 6 + 2×256 bytes. Driver writes here to tell NIC which descriptors are ready.
- **Used ring:** 6 + 8×256 bytes. NIC writes here when it consumes a descriptor.

Queue 0 = RX, Queue 1 = TX.

### RX Queue Setup

Pre-fill all 256 RX descriptor slots with receive buffers:
```c
/* Each RX buffer: 1 page (4096 bytes), 1536 bytes used */
for (int i = 0; i < 256; i++) {
    uint64_t phys = pmm_alloc_page();
    desc[i].addr  = phys;
    desc[i].len   = 1536;
    desc[i].flags = VIRTQ_DESC_F_WRITE; /* NIC writes into this buffer */
    avail->ring[i] = i;
}
avail->idx = 256;
```

### TX Path

TX bounce buffer layout (per slot): `[virtio_net_hdr (10 bytes zero)][Ethernet frame]`.

```c
int virtio_net_send(netdev_t *dev, const void *pkt, uint16_t len) {
    /* 1. Zero the 10-byte virtio_net_hdr at start of TX bounce buffer,
     *    then copy Ethernet frame at offset 10.
     *    Total bytes to NIC: 10 + len. */
    uint8_t *buf = p->tx_virt[p->tx_head & 255];
    memset(buf, 0, 10);               /* virtio_net_hdr (all zeros = no offload) */
    memcpy(buf + 10, pkt, len);
    uint16_t total = 10 + len;
    /* 2. Fill TX descriptor: addr=phys, len=total, flags=0 (single segment) */
    /* 3. Write descriptor index to avail->ring[avail->idx & 255] */
    /* 4. avail->idx++ (compiler barrier: __asm__ volatile("" ::: "memory")) */
    /* 5. sfence — required by virtio 1.0 section 2.6.13.2 before MMIO notify */
    __asm__ volatile("sfence" ::: "memory");
    /* 6. Write queue index (1 for TX) to notify doorbell MMIO address */
    /* 7. Poll used ring until NIC signals completion */
    for (int i = 0; i < 100000; i++) {
        if (p->tx_used->idx != p->tx_last_used) {
            p->tx_last_used++;
            return 0; /* TX complete */
        }
    }
    return -1; /* -EIO: timeout (link down?) */
}
```

### RX Poll (from PIT)

```c
void virtio_net_poll(netdev_t *dev) {
    virtio_priv_t *p = dev->priv;
    while (p->rx_last_used != p->rx_used->idx) {
        uint32_t id  = p->rx_used->ring[p->rx_last_used & 255].id;
        uint32_t len = p->rx_used->ring[p->rx_last_used & 255].len;
        void *buf = p->rx_virt[id]; /* virtual address of receive buffer */
        /* skip 10-byte virtio_net_hdr at start of buffer.
         * Without VIRTIO_NET_F_MRG_RXBUF, the header is 10 bytes:
         * flags(1) + gso_type(1) + hdr_len(2) + gso_size(2) + csum_start(2) + csum_offset(2). */
        netdev_rx_deliver(dev, (uint8_t *)buf + 10, len - 10);
        /* return descriptor to available ring */
        p->rx_avail->ring[p->rx_avail->idx & 255] = id;
        p->rx_avail->idx++;
        p->rx_last_used++;
    }
}
```

### Init Sequence

1. Find device via PCI vendor/device scan
2. Walk capability list, locate COMMON_CFG, NOTIFY_CFG, DEVICE_CFG
3. Map BARs via KVA
4. Write COMMON_CFG: reset → ACKNOWLEDGE → DRIVER → negotiate features (VIRTIO_NET_F_MAC) → FEATURES_OK → DRIVER_OK
5. Read MAC from DEVICE_CFG bytes 0–5
6. Allocate and configure RX virtqueue (index 0)
7. Allocate and configure TX virtqueue (index 1)
8. Register `netdev_t` as `"eth0"`
9. `printk("[NET] OK: virtio-net eth0 mac=%02x:%02x:%02x:%02x:%02x:%02x\n", ...)`

### QEMU Command for Tests

```
-machine q35 \
-device virtio-net-pci,netdev=n0,disable-legacy=on \
-netdev user,id=n0
```

`disable-legacy=on` forces modern (MMIO) transport. QEMU user networking (SLIRP) requires no host configuration.

---

## Testing

### `tests/test_virtio_net.py`

Boots with q35 + virtio-net + SLIRP. Waits for `[NET] OK` line in serial output. Phase 24 does not test actual packet transmission — that is Phase 25.

```python
assert "[NET] OK: virtio-net eth0" in output
```

### `make test` invariance

`make test` uses `-machine pc`. `virtio_net_init()` finds no matching PCI device and returns immediately without printing. `tests/expected/boot.txt` is unchanged.

---

## Forward-Looking Constraints

**`netdev_rx_deliver()` is a stub in Phase 24.** It silently discards the frame. No printk — unexpected serial output during `make run` (where SLIRP sends ARPs, mDNS, etc.) would spam the console. Phase 25 replaces this stub with `eth_rx()` dispatch.

**Single TX bounce buffer.** TX is serialized — one frame in flight at a time. Concurrent sends from multiple processes require a TX ring with proper head/tail tracking. Deferred to Phase 25+.

**No interrupt-driven RX.** Polling at 100Hz adds ~1–2µs per tick. MSI-X driven RX is v2.0 work.

**`netdev_rx_deliver` is ISR-safe but not SMP-safe.** Single-core assumption throughout. Adding SMP requires per-CPU RX queues and lock-free ring buffers.
