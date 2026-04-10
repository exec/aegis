/* xhci.h — xHCI USB host controller driver
 *
 * xHCI 1.2 compliant. Supports device enumeration and interrupt IN
 * transfers for HID boot-protocol keyboards. Polling-based (no MSI).
 */
#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

/* Capability Registers (BAR0 base) */
typedef struct __attribute__((packed)) {
    uint8_t  caplength;       /* 0x00: Capability Register Length */
    uint8_t  reserved;
    uint16_t hciversion;      /* 0x02: Interface Version Number */
    uint32_t hcsparams1;      /* 0x04: Structural Parameters 1 */
    uint32_t hcsparams2;      /* 0x08: Structural Parameters 2 */
    uint32_t hcsparams3;      /* 0x0C: Structural Parameters 3 */
    uint32_t hccparams1;      /* 0x10: Capability Parameters 1 */
    uint32_t dboff;           /* 0x14: Doorbell Offset */
    uint32_t rtsoff;          /* 0x18: Runtime Register Space Offset */
    uint32_t hccparams2;      /* 0x1C: Capability Parameters 2 */
} xhci_cap_regs_t;

/* Operational Registers (BAR0 + CAPLENGTH) */
typedef struct __attribute__((packed)) {
    uint32_t usbcmd;          /* 0x00: USB Command */
    uint32_t usbsts;          /* 0x04: USB Status */
    uint32_t pagesize;        /* 0x08: Page Size */
    uint8_t  reserved1[8];
    uint32_t dnctrl;          /* 0x14: Device Notification Control */
    uint64_t crcr;            /* 0x18: Command Ring Control */
    uint8_t  reserved2[16];
    uint64_t dcbaap;          /* 0x30: Device Context Base Address Array Pointer */
    uint32_t config;          /* 0x38: Configure */
} xhci_op_regs_t;

/* Port Status and Control Register */
typedef struct __attribute__((packed)) {
    uint32_t portsc;          /* Port Status and Control */
    uint32_t portpmsc;        /* Port Power Management Status and Control */
    uint32_t portli;          /* Port Link Info */
    uint32_t porthlpmc;       /* Port Hardware LPM Control */
} xhci_port_regs_t;

/* Transfer Request Block (TRB) — 16 bytes */
typedef struct __attribute__((packed)) {
    uint64_t param;           /* Parameter (address or inline data) */
    uint32_t status;          /* Status / Transfer Length / Interrupter Target */
    uint32_t control;         /* Cycle bit [0], Type [15:10], flags */
} xhci_trb_t;

/* TRB Types */
#define XHCI_TRB_NORMAL            1
#define XHCI_TRB_SETUP             2
#define XHCI_TRB_DATA              3
#define XHCI_TRB_STATUS            4
#define XHCI_TRB_LINK              6
#define XHCI_TRB_ENABLE_SLOT       9
#define XHCI_TRB_ADDRESS_DEVICE    11
#define XHCI_TRB_CONFIGURE_EP      12
#define XHCI_TRB_EVALUATE_CTX      13
#define XHCI_TRB_NOOP              23
#define XHCI_TRB_TRANSFER_EVENT    32
#define XHCI_TRB_CMD_COMPLETION    33
#define XHCI_TRB_PORT_STATUS_CHG   34

/* TRB additional types */
#define XHCI_TRB_DISABLE_SLOT      10

/* USBCMD bits */
#define XHCI_CMD_RS                (1u << 0)
#define XHCI_CMD_HCRST             (1u << 1)
#define XHCI_CMD_INTE              (1u << 2)

/* USBSTS bits */
#define XHCI_STS_HCH               (1u << 0)   /* Host Controller Halted */
#define XHCI_STS_HSE               (1u << 2)   /* Host System Error */
#define XHCI_STS_EINT              (1u << 3)   /* Event Interrupt */
#define XHCI_STS_CNR               (1u << 11)  /* Controller Not Ready */
#define XHCI_STS_HCE               (1u << 12)  /* Host Controller Error */

/* PORTSC bits */
#define XHCI_PORTSC_CCS            (1u << 0)
#define XHCI_PORTSC_PED            (1u << 1)
#define XHCI_PORTSC_PR             (1u << 4)
#define XHCI_PORTSC_CSC            (1u << 17)  /* Connect Status Change (RW1C) */
#define XHCI_PORTSC_PRC            (1u << 21)

/* USB device types for HID boot protocol */
#define USB_DEV_NONE   0u
#define USB_DEV_KBD    1u
#define USB_DEV_MOUSE  2u

/* Command Ring sizes */
#define XHCI_CMD_RING_SIZE         64
#define XHCI_EVT_RING_SIZE         64
#define XHCI_TRANSFER_RING_SIZE    64

/* Initialize xHCI controller. Safe to call when no xHCI device present.
 * Prints [XHCI] OK or skip message. */
void xhci_init(void);

/* Poll event ring for completed transfers. Called from PIT handler at 100Hz. */
void xhci_poll(void);

/* Schedule an interrupt IN transfer for a device slot + endpoint.
 * Used by USB HID driver to receive keyboard reports. */
int xhci_schedule_interrupt_in(uint8_t slot_id, uint8_t ep_id,
                                uint64_t buf_phys, uint32_t buf_len);

#endif /* XHCI_H */
