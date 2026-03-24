/* virtio_net.h — virtio 1.0 modern NIC register layout and virtqueue structs
 *
 * Only the modern (MMIO via PCI capability) transport is supported.
 * No legacy port-I/O path. Port I/O is restricted to kernel/arch/x86_64/.
 *
 * References:
 *   - Virtual I/O Device (VIRTIO) Version 1.0, OASIS Standard 2016-01-11
 *   - Section 5.1: Network Device
 *   - Section 2.6: Virtqueues
 *   - Section 4.1: Virtio Over PCI Bus
 */
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

/* ── PCI identity ──────────────────────────────────────────────────────────── */
#define VIRTIO_VENDOR_ID      0x1AF4u
#define VIRTIO_NET_DEVICE_MODERN  0x1041u   /* virtio 1.0 network device */
#define VIRTIO_NET_DEVICE_LEGACY  0x1000u   /* transitional (also modern-capable) */

/* ── PCI capability sub-types (virtio spec §4.1.4) ────────────────────────── */
#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u

/* ── Device status bits (virtio spec §2.1) ────────────────────────────────── */
#define VIRTIO_STATUS_RESET        0x00u
#define VIRTIO_STATUS_ACKNOWLEDGE  0x01u
#define VIRTIO_STATUS_DRIVER       0x02u
#define VIRTIO_STATUS_FEATURES_OK  0x08u
#define VIRTIO_STATUS_DRIVER_OK    0x04u
#define VIRTIO_STATUS_FAILED       0x80u

/* ── Feature bits (virtio-net, §5.1.3) ───────────────────────────────────── */
#define VIRTIO_NET_F_MAC           (1u << 5)   /* device has MAC address */

/* ── virtio_net_hdr (§5.1.6, virtio 1.0 modern path) ─────────────────────── */
/* 12 bytes prepended to every TX frame; 12 bytes at start of every RX buffer.
 *
 * Virtio 1.0 (VIRTIO_F_VERSION_1 negotiated) always uses the extended header
 * with num_buffers, even without VIRTIO_NET_F_MRG_RXBUF.  The legacy
 * (pre-1.0) header was 10 bytes; the modern 1.0 header is 12 bytes.
 * Skipping only 10 bytes on RX would read 2 bytes of header data as Ethernet
 * frame bytes, silently corrupting every received frame. */
#define VIRTIO_NET_HDR_SIZE 12u
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;  /* always 1 without VIRTIO_NET_F_MRG_RXBUF */
} virtio_net_hdr_t;

/* ── Common configuration structure (§4.1.4.3) ───────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    /* queue-specific fields (select queue first via queue_select) */
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;    /* physical address of descriptor table */
    uint64_t queue_driver;  /* physical address of available ring */
    uint64_t queue_device;  /* physical address of used ring */
} virtio_pci_common_cfg_t;

/* ── Virtqueue descriptor (§2.6.5) ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;   /* physical address of buffer */
    uint32_t len;    /* length in bytes */
    uint16_t flags;  /* VIRTQ_DESC_F_NEXT (1), VIRTQ_DESC_F_WRITE (2) */
    uint16_t next;   /* next descriptor index (if NEXT flag set) */
} virtq_desc_t;

#define VIRTQ_DESC_F_NEXT   1u
#define VIRTQ_DESC_F_WRITE  2u  /* NIC writes into this buffer (RX) */

/* ── Virtqueue available ring (§2.6.6) ───────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256];
    uint16_t used_event;
} virtq_avail_t;

/* ── Virtqueue used ring element (§2.6.8) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

/* ── Virtqueue used ring (§2.6.8) ────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[256];
    uint16_t avail_event;
} virtq_used_t;

/* ── Driver public API ────────────────────────────────────────────────────── */

/* virtio_net_init — scan PCIe for a virtio-net device, initialize it,
 * and register as "eth0". Returns silently (no printk) if no device found.
 * Called from kernel_main() after xhci_init(). */
void virtio_net_init(void);

#endif /* VIRTIO_NET_H */
