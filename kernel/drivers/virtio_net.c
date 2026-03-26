/* virtio_net.c — virtio 1.0 modern network device driver
 *
 * Transport: PCI capability-list MMIO only (modern transport, §4.1).
 * No legacy port I/O. No MSI-X (polling only at 100 Hz via PIT).
 *
 * Memory model:
 *   - PCI config space: accessed via PCIe ECAM (pcie.h)
 *   - BAR MMIO: mapped with kva_alloc_pages + vmm_map_page (PWT|PCD = no-cache)
 *   - DMA buffers: pmm_alloc_page() for physical address; kva_alloc_pages()
 *     for virtual address, mapped via vmm_map_page
 *
 * Virtqueue layout (per queue, 256 entries):
 *   - Descriptor table:  sizeof(virtq_desc_t)*256 = 4096 bytes (1 page)
 *   - Available ring:    sizeof(virtq_avail_t)    = 6+512+2 = 520 bytes (1 page)
 *   - Used ring:         sizeof(virtq_used_t)     = 6+2048+2 = 2056 bytes (1 page)
 *
 * Queue 0 = RX (NIC writes frames in), Queue 1 = TX (driver writes frames out).
 */
#include "virtio_net.h"
#include "../net/netdev.h"
#include "arch.h"
#include "../arch/x86_64/pcie.h"
#include "kva.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

/* String helpers — no libc in kernel */
static void
_memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = dst;
    while (n--)
        *p++ = (uint8_t)val;
}

static void
_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
}

/* ── BAR mapping helper ─────────────────────────────────────────────────────
 * Maps n_pages of a BAR region (physical base pa) into KVA with PWT+PCD
 * (no-cache) flags. Returns the virtual base address.
 * flags: 0x1Bu = Present(1)|Write(2)|PWT(8)|PCD(16) = no-cache MMIO mapping. */
#define VIRTIO_MAP_FLAGS 0x1Bu

static uintptr_t
map_bar_region(uint64_t pa, uint32_t n_pages)
{
    uintptr_t va = (uintptr_t)kva_alloc_pages(n_pages);
    uint32_t i;
    for (i = 0; i < n_pages; i++) {
        uintptr_t page_va = va + (uint64_t)i * 4096;
        /* kva_alloc_pages already mapped each page to a PMM frame;
         * unmap first so vmm_map_page does not panic on a double-map.
         * SAFETY: page_va is present (kva_alloc_pages guarantees this);
         * vmm_unmap_page succeeds; then vmm_map_page installs the BAR PA. */
        vmm_unmap_page(page_va);
        vmm_map_page(page_va, pa + (uint64_t)i * 4096, VIRTIO_MAP_FLAGS);
    }
    return va;
}

/* ── Per-device private state ───────────────────────────────────────────── */
#define VIRTQ_SIZE  256u

typedef struct {
    /* Common config MMIO pointer */
    volatile virtio_pci_common_cfg_t *common;

    /* Notify doorbell MMIO base + per-queue multiplier */
    volatile uint32_t *notify_base;
    uint32_t           notify_off_mult;

    /* RX queue (index 0) */
    volatile virtq_desc_t  *rx_desc;
    volatile virtq_avail_t *rx_avail;
    volatile virtq_used_t  *rx_used;
    void                   *rx_virt[VIRTQ_SIZE];  /* virtual addresses of RX bufs */
    uint16_t                rx_notify_off;
    uint16_t                rx_last_used;

    /* TX queue (index 1) */
    volatile virtq_desc_t  *tx_desc;
    volatile virtq_avail_t *tx_avail;
    volatile virtq_used_t  *tx_used;
    uint8_t                *tx_virt[VIRTQ_SIZE];  /* virtual addresses of TX bounce bufs */
    uint16_t                tx_notify_off;
    uint16_t                tx_head;
    uint16_t                tx_last_used;
} virtio_priv_t;

/* Static storage for one device (Phase 24 supports a single virtio-net) */
static virtio_priv_t  s_priv;
static netdev_t       s_dev;

/* ── PCI capability walker ──────────────────────────────────────────────────
 * Walk the PCI capability list for the device at (bus, dev, fn).
 * Find virtio vendor-specific capabilities (type 0x09) and locate:
 *   sub-type 1 (COMMON_CFG), 2 (NOTIFY_CFG), 4 (DEVICE_CFG).
 *
 * Capability layout (virtio spec §4.1.4):
 *   offset+0:  cap_id  (1 byte) — 0x09 = vendor-specific
 *   offset+1:  cap_next(1 byte) — next cap offset in config space
 *   offset+2:  cap_len (1 byte)
 *   offset+3:  cfg_type(1 byte) — virtio sub-type (1/2/4)
 *   offset+4:  bar     (1 byte) — which BAR
 *   offset+5:  padding (3 bytes)
 *   offset+8:  offset  (4 bytes) — byte offset within BAR
 *   offset+12: length  (4 bytes) — length in bytes
 * For NOTIFY_CFG (sub-type 2), an extra 4 bytes follow at offset+16:
 *   offset+16: notify_off_multiplier (4 bytes)
 */
static void
walk_caps(const pcie_device_t *d,
          uint8_t *out_common_bar, uint32_t *out_common_off,
          uint8_t *out_notify_bar, uint32_t *out_notify_off,
                                   uint32_t *out_notify_mult,
          uint8_t *out_device_bar, uint32_t *out_device_off)
{
    /* capabilities pointer is at PCI config offset 0x34 */
    uint8_t cap_ptr = (uint8_t)pcie_read8(d->bus, d->dev, d->fn, 0x34) & 0xFCu;

    while (cap_ptr != 0) {
        uint8_t cap_id   = pcie_read8(d->bus, d->dev, d->fn, cap_ptr + 0);
        uint8_t cap_next = pcie_read8(d->bus, d->dev, d->fn, cap_ptr + 1);

        if (cap_id == 0x09u) {  /* PCI vendor-specific capability */
            uint8_t  cfg_type = pcie_read8 (d->bus, d->dev, d->fn, cap_ptr + 3);
            uint8_t  bar      = pcie_read8 (d->bus, d->dev, d->fn, cap_ptr + 4);
            uint32_t off      = pcie_read32(d->bus, d->dev, d->fn, cap_ptr + 8);

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                *out_common_bar = bar;
                *out_common_off = off;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                *out_notify_bar  = bar;
                *out_notify_off  = off;
                *out_notify_mult = pcie_read32(d->bus, d->dev, d->fn, cap_ptr + 16);
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                *out_device_bar = bar;
                *out_device_off = off;
                break;
            default:
                break;
            }
        }
        cap_ptr = cap_next & 0xFCu;
    }
}

/* ── DMA page allocator helper ──────────────────────────────────────────────
 * Allocates one PMM page, maps it into KVA (cached, Present|Write),
 * zeroes it, returns both physical and virtual addresses. */
static void
alloc_dma_page(uint64_t *phys_out, uintptr_t *virt_out)
{
    /* kva_alloc_pages allocates a PMM page AND maps it to the returned VA.
     * Use kva_page_phys to retrieve the physical address rather than calling
     * pmm_alloc_page() separately (which would require an unmap+remap cycle
     * identical to map_bar_region and would leak the extra PMM frame). */
    uintptr_t va = (uintptr_t)kva_alloc_pages(1);
    uint64_t  pa = kva_page_phys((void *)va);
    _memset((void *)va, 0, 4096);
    *phys_out = pa;
    *virt_out = va;
}

/* ── Forward declarations ──────────────────────────────────────────────────*/
static int  virtio_net_send(netdev_t *dev, const void *pkt, uint16_t len);
static void virtio_net_poll(netdev_t *dev);

/* ── virtio_net_init ─────────────────────────────────────────────────────── */
void
virtio_net_init(void)
{
    int i;
    const pcie_device_t *found = NULL;

    /* Step 1: scan PCIe device list for virtio-net (vendor 0x1AF4,
     * device 0x1041 modern or 0x1000 legacy-transitional). */
    int count = pcie_device_count();
    for (i = 0; i < count; i++) {
        const pcie_device_t *d = &pcie_get_devices()[i];
        if (d->vendor_id == VIRTIO_VENDOR_ID &&
            (d->device_id == VIRTIO_NET_DEVICE_MODERN ||
             d->device_id == VIRTIO_NET_DEVICE_LEGACY)) {
            found = d;
            break;
        }
    }
    if (!found)
        return; /* silent: make test uses -machine pc, no virtio-net present */

    /* Step 2: walk PCI capability list. */
    uint8_t  common_bar = 0, notify_bar = 0, device_bar = 0;
    uint32_t common_off = 0, notify_off = 0, notify_mult = 0, device_off = 0;

    walk_caps(found,
              &common_bar, &common_off,
              &notify_bar, &notify_off, &notify_mult,
              &device_bar, &device_off);

    /* Step 3: map BARs into KVA (no-cache MMIO).
     * pcie_device_t.bar[] contains decoded 64-bit BAR base addresses
     * populated by pcie_init() — no need to re-read from config space. */
    uint64_t common_pa = found->bar[common_bar] + common_off;
    uint64_t notify_pa = found->bar[notify_bar] + notify_off;
    uint64_t device_pa = found->bar[device_bar] + device_off;

    uintptr_t common_va = map_bar_region(common_pa & ~0xFFFULL, 1)
                          + (common_pa & 0xFFFULL);
    uintptr_t notify_va = map_bar_region(notify_pa & ~0xFFFULL, 1)
                          + (notify_pa & 0xFFFULL);
    uintptr_t device_va = map_bar_region(device_pa & ~0xFFFULL, 1)
                          + (device_pa & 0xFFFULL);

    volatile virtio_pci_common_cfg_t *cfg =
        (volatile virtio_pci_common_cfg_t *)common_va;
    volatile uint32_t *notify = (volatile uint32_t *)notify_va;
    volatile uint8_t  *devcfg = (volatile uint8_t  *)device_va;

    s_priv.common        = cfg;
    s_priv.notify_base   = notify;
    s_priv.notify_off_mult = notify_mult;

    /* Step 4: virtio feature negotiation (§3.1.1).
     *   RESET → ACKNOWLEDGE → DRIVER → negotiate → FEATURES_OK → DRIVER_OK */
    cfg->device_status = VIRTIO_STATUS_RESET;
    cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    cfg->device_status = (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE |
                                   VIRTIO_STATUS_DRIVER);

    /* Read device features (low 32 bits). Offer only VIRTIO_NET_F_MAC. */
    cfg->device_feature_select = 0;
    uint32_t dev_feat = cfg->device_feature;
    (void)dev_feat; /* we only care that MAC is supported */

    cfg->driver_feature_select = 0;
    cfg->driver_feature        = VIRTIO_NET_F_MAC;
    cfg->driver_feature_select = 1;
    /* VIRTIO_F_VERSION_1 = bit 32 = bit 0 of high feature word.
     * Non-transitional devices (0x1041) require this bit to be negotiated;
     * QEMU rejects FEATURES_OK if it is absent on the modern path. */
    cfg->driver_feature        = (1u << 0); /* VIRTIO_F_VERSION_1 */

    cfg->device_status = (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE |
                                   VIRTIO_STATUS_DRIVER      |
                                   VIRTIO_STATUS_FEATURES_OK);

    /* Check FEATURES_OK was accepted */
    if (!(cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        cfg->device_status = VIRTIO_STATUS_FAILED;
        return;
    }

    /* Step 5: read MAC from device config bytes 0–5. */
    for (i = 0; i < 6; i++)
        s_dev.mac[i] = devcfg[i];

    /* Step 6 & 7: set up RX queue (0) and TX queue (1). */
    uint32_t q;
    for (q = 0; q <= 1; q++) {
        cfg->queue_select = (uint16_t)q;
        cfg->queue_size   = VIRTQ_SIZE;

        uint64_t  desc_pa, avail_pa, used_pa;
        uintptr_t desc_va, avail_va, used_va;

        alloc_dma_page(&desc_pa,  &desc_va);
        alloc_dma_page(&avail_pa, &avail_va);
        alloc_dma_page(&used_pa,  &used_va);

        cfg->queue_desc   = desc_pa;
        cfg->queue_driver = avail_pa;
        cfg->queue_device = used_pa;

        uint16_t noff = cfg->queue_notify_off;
        cfg->queue_enable = 1;

        volatile virtq_desc_t  *desc  = (volatile virtq_desc_t  *)desc_va;
        volatile virtq_avail_t *avail = (volatile virtq_avail_t *)avail_va;
        volatile virtq_used_t  *used  = (volatile virtq_used_t  *)used_va;

        if (q == 0) {
            /* RX queue: pre-fill all 256 slots with 1-page receive buffers. */
            for (i = 0; i < (int)VIRTQ_SIZE; i++) {
                uint64_t  buf_pa;
                uintptr_t buf_va;
                alloc_dma_page(&buf_pa, &buf_va);
                s_priv.rx_virt[i] = (void *)buf_va;
                desc[i].addr  = buf_pa;
                desc[i].len   = 1536;
                desc[i].flags = VIRTQ_DESC_F_WRITE; /* NIC writes into buf */
                desc[i].next  = 0;
                avail->ring[i] = (uint16_t)i;
            }
            /* Publish all 256 buffers to the NIC */
            __asm__ volatile("" ::: "memory"); /* compiler barrier */
            avail->idx = VIRTQ_SIZE;
            /* Kick RX queue so device notices the pre-filled buffers.
             * virtio spec §2.7.13: driver SHOULD send notification after
             * adding available buffers. Required sfence before MMIO write. */
            arch_wmb();
            notify[noff * notify_mult / 4u] = 0u; /* queue index 0 = RX */

            s_priv.rx_desc      = desc;
            s_priv.rx_avail     = avail;
            s_priv.rx_used      = used;
            s_priv.rx_notify_off = noff;
            s_priv.rx_last_used  = 0;
        } else {
            /* TX queue: allocate one bounce buffer per slot.
             * Each bounce buffer = 12 bytes (virtio_net_hdr v1.0) + 1514 bytes max
             * Ethernet frame = 1526 bytes. One page (4096) per slot is ample. */
            for (i = 0; i < (int)VIRTQ_SIZE; i++) {
                uint64_t  buf_pa;
                uintptr_t buf_va;
                alloc_dma_page(&buf_pa, &buf_va);
                s_priv.tx_virt[i] = (uint8_t *)buf_va;
                /* Pre-wire descriptor physical address; len and flags set at send time */
                desc[i].addr = buf_pa;
            }

            s_priv.tx_desc       = desc;
            s_priv.tx_avail      = avail;
            s_priv.tx_used       = used;
            s_priv.tx_notify_off  = noff;
            s_priv.tx_head        = 0;
            s_priv.tx_last_used   = 0;
        }
    }

    /* Step 4 continued: set DRIVER_OK to activate device. */
    cfg->device_status = (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE |
                                   VIRTIO_STATUS_DRIVER      |
                                   VIRTIO_STATUS_FEATURES_OK |
                                   VIRTIO_STATUS_DRIVER_OK);

    /* Re-kick RX queue after DRIVER_OK.
     * The RX kick in the queue setup loop above fires before DRIVER_OK is set.
     * QEMU may defer processing available buffers until the device is fully
     * activated.  Kick again now that DRIVER_OK is set so the device picks up
     * the 256 pre-filled descriptors and starts delivering received frames. */
    arch_wmb();
    s_priv.notify_base[s_priv.rx_notify_off * s_priv.notify_off_mult / 4u] = 0u;

    /* Step 8: register netdev. */
    s_dev.name[0]='e'; s_dev.name[1]='t'; s_dev.name[2]='h';
    s_dev.name[3]='0'; s_dev.name[4]='\0';
    s_dev.mtu  = 1500;
    s_dev.send = virtio_net_send;
    s_dev.poll = virtio_net_poll;
    s_dev.priv = &s_priv;

    netdev_register(&s_dev);

    /* Step 9: announce. */
    printk("[NET] OK: virtio-net eth0 mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           s_dev.mac[0], s_dev.mac[1], s_dev.mac[2],
           s_dev.mac[3], s_dev.mac[4], s_dev.mac[5]);
}

/* ── virtio_net_send ─────────────────────────────────────────────────────────
 * Transmit one Ethernet frame (pkt, len bytes) via virtqueue 1.
 *
 * TX bounce buffer layout:
 *   [0..11]  = virtio_net_hdr_t (12 bytes, all zero = no offload)
 *   [12..12+len-1] = Ethernet frame
 *
 * Protocol (virtio spec §2.6.13):
 *   1. Fill bounce buffer.
 *   2. Fill TX descriptor (single segment, no NEXT/WRITE flags).
 *   3. Write descriptor index into avail->ring[avail->idx & 255].
 *   4. Increment avail->idx (compiler barrier before increment).
 *   5. sfence — required by §2.6.13.2 before MMIO write.
 *   6. Write queue index (1) to notify doorbell.
 *   7. Poll used ring for up to 100000 iterations for completion.
 */
static int
virtio_net_send(netdev_t *dev, const void *pkt, uint16_t len)
{
    virtio_priv_t *p = (virtio_priv_t *)dev->priv;
    uint16_t slot    = p->tx_head & (uint16_t)(VIRTQ_SIZE - 1);
    uint16_t total   = (uint16_t)(VIRTIO_NET_HDR_SIZE + len);

    /* 1. Zero virtio_net_hdr (12 bytes for virtio 1.0 modern), copy frame. */
    uint8_t *buf = p->tx_virt[slot];
    _memset(buf, 0, VIRTIO_NET_HDR_SIZE);
    _memcpy(buf + VIRTIO_NET_HDR_SIZE, pkt, len);

    /* 2. Set descriptor. flags=0: device reads (not write), single segment. */
    p->tx_desc[slot].len   = total;
    p->tx_desc[slot].flags = 0;
    p->tx_desc[slot].next  = 0;

    /* 3. Publish descriptor into available ring. */
    uint16_t avail_idx = p->tx_avail->idx;
    p->tx_avail->ring[avail_idx & (uint16_t)(VIRTQ_SIZE - 1)] = slot;

    /* 4. Advance avail->idx (compiler barrier prevents reorder). */
    __asm__ volatile("" ::: "memory");
    p->tx_avail->idx = (uint16_t)(avail_idx + 1);

    /* 5. sfence — memory barrier required before MMIO notify doorbell.
     * virtio spec §2.6.13.2: driver MUST issue memory barrier before
     * writing the notification register. */
    arch_wmb();

    /* 6. Notify device: write queue index 1 to doorbell.
     * Doorbell address = notify_base + (queue_notify_off * notify_off_mult / 4). */
    volatile uint32_t *doorbell =
        p->notify_base + (p->tx_notify_off * p->notify_off_mult / 4u);
    *doorbell = 1u; /* queue index 1 = TX */

    /* 7. Poll used ring for completion (sync TX, Phase 24 design). */
    int i;
    for (i = 0; i < 100000; i++) {
        __asm__ volatile("" ::: "memory"); /* re-read used->idx each iteration */
        if (p->tx_used->idx != p->tx_last_used) {
            p->tx_last_used++;
            p->tx_head++;
            return 0;
        }
    }
    /* TX timeout — drain any completions the device may have posted
     * to keep tx_last_used in sync before giving up. */
    while (p->tx_used->idx != p->tx_last_used) {
        p->tx_last_used++;
        p->tx_head++;
    }
    return -1;
}

/* ── virtio_net_poll ─────────────────────────────────────────────────────────
 * Drain the RX used ring. Called from pit_handler via netdev_poll_all() at
 * 100 Hz. Must not block.
 *
 * For each completed RX descriptor:
 *   1. Read (id, len) from used ring.
 *   2. Call netdev_rx_deliver() with the frame (skip 10-byte virtio_net_hdr).
 *   3. Return the descriptor to the available ring so the NIC can reuse it.
 */
static void
virtio_net_poll(netdev_t *dev)
{
    virtio_priv_t *p = (virtio_priv_t *)dev->priv;
    if (p->rx_last_used != p->rx_used->idx) {
        printk("[VNET] RX pkt: last=%u used=%u\n",
               (uint32_t)p->rx_last_used, (uint32_t)p->rx_used->idx);
    }

    while (p->rx_last_used != p->rx_used->idx) {
        __asm__ volatile("" ::: "memory");
        uint16_t slot = (uint16_t)(p->rx_last_used & (uint16_t)(VIRTQ_SIZE - 1));
        uint32_t id   = p->rx_used->ring[slot].id;
        uint32_t rlen = p->rx_used->ring[slot].len;

        if (id >= VIRTQ_SIZE) {          /* guard against misbehaving device */
            p->rx_last_used++;
            continue;
        }

        /* Skip the 12-byte virtio_net_hdr at the start of the receive buffer.
         * Virtio 1.0 modern always uses a 12-byte header (including num_buffers)
         * regardless of VIRTIO_NET_F_MRG_RXBUF. */
        if (rlen > VIRTIO_NET_HDR_SIZE) {
            void *buf = p->rx_virt[id];
            netdev_rx_deliver(dev, (uint8_t *)buf + VIRTIO_NET_HDR_SIZE,
                              (uint16_t)(rlen - VIRTIO_NET_HDR_SIZE));
        }

        /* Return descriptor to available ring. */
        uint16_t avail_idx = p->rx_avail->idx;
        p->rx_avail->ring[avail_idx & (uint16_t)(VIRTQ_SIZE - 1)] = (uint16_t)id;
        __asm__ volatile("" ::: "memory");
        p->rx_avail->idx = (uint16_t)(avail_idx + 1u);

        p->rx_last_used++;
    }

    /* Always write to the RX notify doorbell (even if we received no frames).
     *
     * In TCG mode (no KVM), the guest vCPU and QEMU's virtio/SLIRP backend
     * run on the same host thread.  QEMU can only inject RX frames when the
     * guest causes a VM-exit (MMIO access, port I/O, etc.).  If we only kick
     * when descriptors were returned, the ARP busy-poll loop never writes to
     * MMIO and QEMU never gets the CPU — the ARP reply is never delivered.
     *
     * Writing queue index 0 here forces a VM-exit every poll() call, giving
     * QEMU time to process SLIRP events and inject pending RX frames.
     * The write also re-notifies the device that RX buffers are available,
     * which is correct per virtio spec §2.7.13 regardless of whether we
     * returned any buffers in this call.
     */
    arch_wmb();
    p->notify_base[p->rx_notify_off * p->notify_off_mult / 4u] = 0u;
}
