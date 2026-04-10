/* xhci.c — xHCI USB host controller driver (Phase 22)
 *
 * Implements xHCI 1.2 controller initialization, device enumeration for
 * HID boot-protocol keyboards, and polling-based transfer handling.
 *
 * Init sequence:
 *   1. Locate xHCI controller via pcie_find_device(0x0C, 0x03, 0x30)
 *   2. Map BAR0 MMIO (16 pages = 64KB, uncached)
 *   3. Stop + reset controller
 *   4. Allocate and wire up DCBAA, Command Ring, Event Ring (ERST)
 *   5. Start controller (USBCMD.RS = 1)
 *   6. Enumerate ports: reset, Enable Slot, Address Device, Configure EP
 *   7. Schedule first interrupt IN transfer per keyboard slot
 *
 * Polling: xhci_poll() is called from the PIT ISR at 100 Hz.
 * No MSI/MSI-X — event ring is polled, not interrupt-driven.
 */
#include "xhci.h"
#include "arch.h"
#include "pcie.h"
#include "vmm.h"
#include "kva.h"
#include "pmm.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

/* Forward-declare the USB HID report handler.  usb_hid.h is defined in
 * Task 3; including it here would create a forward-dependency during phased
 * development.  The linker resolves this symbol when usb_hid.c is compiled
 * and linked in Task 4. */
void usb_hid_process_report(const uint8_t *report, uint32_t len);
void usb_mouse_process_report(const uint8_t *data, uint32_t len);

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/* Number of BAR0 pages to map (16 pages = 64KB).
 * Covers CAP + OP + Runtime + Doorbell registers for QEMU qemu-xhci. */
#define XHCI_BAR0_PAGES      16u

/* Maximum slots we support (hardware MaxSlots may be higher). */
#define XHCI_MAX_SLOTS       32u

/* Endpoint index (doorbell value) for EP1 IN (HID keyboard).
 * xHCI DCI for EP1 IN = 2 * ep_number + direction(IN=1) = 2*1+1 = 3. */
#define XHCI_EP1_IN_DCI      3u

/* Input Context entry size (bytes per context slot).
 * Determined at init from HCCPARAMS1.CSZ (bit 2):
 *   0 = 32-byte contexts (xHCI 0.96/1.0 baseline)
 *   1 = 64-byte contexts (most actual implementations including qemu-xhci)
 * Stored in s_ctx_entry_size during xhci_init. */
static uint32_t s_ctx_entry_size = 32u;
#define XHCI_CTX_ENTRY_SIZE  s_ctx_entry_size

/* USB speed values in Slot Context speed field [PORTSC bits 13:10]. */
#define XHCI_SPEED_FS        1u   /* Full Speed  (12  Mbit/s) */
#define XHCI_SPEED_LS        2u   /* Low  Speed  (1.5 Mbit/s) */
#define XHCI_SPEED_HS        3u   /* High Speed  (480 Mbit/s) */
#define XHCI_SPEED_SS        4u   /* Super Speed (5   Gbit/s) */

/* EP types in Endpoint Context EPType field. */
#define XHCI_EP_TYPE_CTRL    4u
#define XHCI_EP_TYPE_INT_IN  7u

/* Byte offsets of operational register fields (xHCI spec §5.4).
 * Used to avoid taking addresses of packed struct members. */
#define XHCI_OP_USBCMD_OFF   0x00u
#define XHCI_OP_USBSTS_OFF   0x04u

/* -------------------------------------------------------------------------
 * Static state
 * ---------------------------------------------------------------------- */

static int                       s_xhci_active    = 0;
static int                       s_post_boot      = 0;
static uint8_t                  *s_bar0_va         = NULL;
/* SAFETY: s_cap and s_op are volatile MMIO pointers — volatile prevents the
 * compiler from caching hardware register reads/writes. */
static volatile xhci_cap_regs_t *s_cap             = NULL;
static volatile xhci_op_regs_t  *s_op              = NULL;

/* Command Ring */
static xhci_trb_t               *s_cmd_ring        = NULL;
static uint64_t                   s_cmd_ring_phys   = 0;
static int                        s_cmd_cycle       = 1;
static uint32_t                   s_cmd_enqueue     = 0;

/* Event Ring */
/* SAFETY: s_evt_ring is volatile so cycle-bit polls are not optimised away. */
static volatile xhci_trb_t      *s_evt_ring         = NULL;
static uint32_t                   s_evt_dequeue      = 0;
static int                        s_evt_cycle        = 1;

static uint32_t                   s_max_ports        = 0;
static uint32_t                   s_max_slots        = 0;

/* DCBAA */
static uint64_t                   s_dcbaa_phys       = 0;
static uint64_t                  *s_dcbaa             = NULL;

/* Per-slot HID state (index 0 unused; xHCI slots are 1-based) */
static int        s_hid_slots[XHCI_MAX_SLOTS];        /* 1 = active HID device */
static uint8_t    s_hid_slot_type[XHCI_MAX_SLOTS];   /* USB_DEV_NONE/KBD/MOUSE */
static uint8_t    s_slot_port[XHCI_MAX_SLOTS];       /* port_num for each slot */
static uint8_t   *s_hid_buf[XHCI_MAX_SLOTS];          /* VA of 8-byte report */
static uint64_t   s_hid_buf_phys[XHCI_MAX_SLOTS];     /* PA of report buffer */
static xhci_trb_t *s_xfer_ring[XHCI_MAX_SLOTS];       /* transfer ring VA */
static uint64_t    s_xfer_ring_phys[XHCI_MAX_SLOTS];  /* transfer ring PA */
static uint32_t    s_xfer_enqueue[XHCI_MAX_SLOTS];
static int         s_xfer_cycle[XHCI_MAX_SLOTS];

/* -------------------------------------------------------------------------
 * MMIO accessor helpers
 *
 * The op register struct is __attribute__((packed)).  GCC treats
 * -Waddress-of-packed-member as an error, so we must not take the address
 * of any packed struct member.  Instead, compute the byte offset explicitly
 * and cast the base pointer — the fields are at their spec-defined offsets
 * and all are naturally aligned within the 4KB MMIO page.
 * ---------------------------------------------------------------------- */

static inline uint32_t
op_read32(uint32_t byte_off)
{
    /* SAFETY: s_op is a kernel VA mapping xHCI operational MMIO; byte_off is
     * a compile-time constant (XHCI_OP_*_OFF) within the mapped 64KB region.
     * Casting via uint8_t * and reading through volatile uint32_t * ensures
     * a 4-byte MMIO load without touching a packed struct field directly. */
    volatile uint32_t *p =
        (volatile uint32_t *)((volatile uint8_t *)s_op + byte_off);
    return *p;
}

static inline void
op_write32(uint32_t byte_off, uint32_t val)
{
    /* SAFETY: same rationale as op_read32. */
    volatile uint32_t *p =
        (volatile uint32_t *)((volatile uint8_t *)s_op + byte_off);
    *p = val;
}

/* -------------------------------------------------------------------------
 * Spin helpers
 * ---------------------------------------------------------------------- */

/* op_spin_until_set — busy-wait until (op_reg_at_off & mask) != 0.
 * Returns 0 on success, -1 on timeout. */
static int
op_spin_until_set(uint32_t off, uint32_t mask)
{
    uint32_t timeout = 2000000u;
    while (timeout--) {
        if (op_read32(off) & mask)
            return 0;
    }
    return -1;
}

/* op_spin_until_clear — busy-wait until (op_reg_at_off & mask) == 0.
 * Returns 0 on success, -1 on timeout. */
static int
op_spin_until_clear(uint32_t off, uint32_t mask)
{
    uint32_t timeout = 2000000u;
    while (timeout--) {
        if (!(op_read32(off) & mask))
            return 0;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Page allocator helper
 * ---------------------------------------------------------------------- */

/* alloc_page — allocate one 4KB zeroed page, return VA; set *phys_out.
 * Follows the nvme.c alloc_queue_page() pattern exactly. */
static void *
alloc_page(uint64_t *phys_out)
{
    void *va = kva_alloc_pages(1);
    /* SAFETY: kva_alloc_pages returns a kernel VA for a PMM-allocated page;
     * zeroing via __builtin_memset is safe. */
    __builtin_memset(va, 0, 4096);
    *phys_out = kva_page_phys(va);
    return va;
}

/* -------------------------------------------------------------------------
 * Command Ring helpers
 * ---------------------------------------------------------------------- */

/* enqueue_cmd — place a TRB on the command ring, advance the tail.
 * Stamps the producer cycle bit into control[0].  At position
 * XHCI_CMD_RING_SIZE-1 we place a Link TRB and flip the cycle bit. */
static void
enqueue_cmd(uint64_t param, uint32_t status, uint32_t type_and_flags)
{
    xhci_trb_t *trb = &s_cmd_ring[s_cmd_enqueue];
    trb->param   = param;
    trb->status  = status;
    trb->control = type_and_flags | (uint32_t)s_cmd_cycle;

    s_cmd_enqueue++;
    if (s_cmd_enqueue == XHCI_CMD_RING_SIZE - 1u) {
        /* Rewrite Link TRB with current cycle bit, then toggle */
        xhci_trb_t *link = &s_cmd_ring[s_cmd_enqueue];
        link->param   = s_cmd_ring_phys;
        link->status  = 0;
        link->control = (uint32_t)(XHCI_TRB_LINK << 10) |
                        (1u << 1) |              /* Toggle Cycle bit */
                        (uint32_t)s_cmd_cycle;
        s_cmd_cycle  ^= 1;
        s_cmd_enqueue = 0;
    }
}

/* ring_cmd_doorbell — notify controller of new command ring entries. */
static void
ring_cmd_doorbell(void)
{
    /* SAFETY: s_bar0_va + cap->dboff is the doorbell array start;
     * doorbell 0 is the host-controller command doorbell. */
    volatile uint32_t *db =
        (volatile uint32_t *)(s_bar0_va + s_cap->dboff);
    /* sfence: all TRB writes must be globally visible before doorbell. */
    arch_wmb();
    db[0] = 0;
}

/* update_erdp — write current event dequeue pointer to interrupter 0.
 * ERDP is at runtime_base + 0x20 (interrupter 0) + 0x18 (ERDP within
 * interrupter register set), and it must hold a PHYSICAL address — the
 * controller uses it for DMA. The previous version wrote a kernel virtual
 * address AND used the wrong offset (0x10 instead of 0x18, which would
 * land in ERSTBA). */
static void
update_erdp(void)
{
    uint8_t *rts = s_bar0_va + s_cap->rtsoff;
    volatile uint64_t *erdp =
        (volatile uint64_t *)(rts + 0x20u + 0x18u);
    /* s_evt_ring is at the start of an alloc_page'd region; the page's
     * physical base is captured at init time as evt_ring_phys.  We don't
     * store it as a per-driver field, so recompute via kva_page_phys
     * (cheap walk of the kernel page tables). */
    *erdp = kva_page_phys((void *)&s_evt_ring[s_evt_dequeue]);
}

/* poll_cmd_completion — wait for a CMD_COMPLETION event on the event ring.
 * Skips any other event types (Port Status Change, Transfer events) that
 * may have been posted before the command completion arrives.
 * Returns the slot_id from bits [31:24] of the event control field on
 * success (TRB_COMPLETION_SUCCESS, cc=1), or 0 on timeout/error. */
static uint8_t
poll_cmd_completion(void)
{
    uint32_t timeout = 2000000u;
    while (timeout--) {
        volatile xhci_trb_t *trb = &s_evt_ring[s_evt_dequeue];
        /* SAFETY: trb is a volatile pointer into a kva-mapped page; the
         * volatile qualifier ensures each cycle-bit read goes to memory. */
        uint32_t ctrl = trb->control;
        if ((ctrl & 1u) != (uint32_t)s_evt_cycle)
            continue;   /* no new event yet */

        uint32_t trb_type = (ctrl >> 10) & 0x3Fu;
        uint8_t  slot_id  = (uint8_t)((ctrl >> 24) & 0xFFu);
        uint8_t  cc       = (uint8_t)((trb->status >> 24) & 0xFFu);

        s_evt_dequeue++;
        if (s_evt_dequeue >= XHCI_EVT_RING_SIZE) {
            s_evt_dequeue = 0;
            s_evt_cycle  ^= 1;
        }
        update_erdp();

        if (trb_type == XHCI_TRB_CMD_COMPLETION) {
            if (cc == 1u)
                return slot_id;
            printk("[XHCI] cmd_completion fail: cc=%u slot=%u\n",
                   (unsigned)cc, (unsigned)slot_id);
            return 0;
        }
        /* Other event types (port status change, etc.) — skip and keep
         * looking for the CMD_COMPLETION we issued. */
    }
    printk("[XHCI] cmd_completion TIMEOUT\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Device enumeration helpers
 * ---------------------------------------------------------------------- */

/* issue_enable_slot — send Enable Slot command; return assigned slot_id
 * (1-based) or 0 on failure. */
static uint8_t
issue_enable_slot(void)
{
    enqueue_cmd(0, 0, (uint32_t)(XHCI_TRB_ENABLE_SLOT << 10));
    ring_cmd_doorbell();
    return poll_cmd_completion();
}

/* issue_address_device — build a minimal Input Context and issue Address
 * Device command for the given slot.
 *
 * Input Context layout (Input Control Ctx + Slot Ctx + EP0 Ctx):
 *   [0]  Input Control Context  (32B): Add A0(slot) + A1(EP0)
 *   [1]  Slot Context           (32B): Speed, ContextEntries=1, RootHubPort
 *   [2]  EP0 Context            (32B): CErr=3, EPType=Control, MaxPacketSize=8
 *
 * Returns 0 on success, -1 on failure. */
static int
issue_address_device(uint8_t slot_id, uint8_t port_num, uint8_t speed)
{
    uint64_t  ictx_phys;
    uint8_t  *ictx = (uint8_t *)alloc_page(&ictx_phys);

    /* Input Control Context: Drop=0, Add=0x3 (A0=slot + A1=EP0) */
    ((volatile uint32_t *)ictx)[0] = 0u;
    ((volatile uint32_t *)ictx)[1] = 0x3u;

    /* Slot Context (xHCI 1.0 §6.2.2):
     *   dword0: Route[19:0], Speed[23:20], MTT[25], Hub[26], ContextEntries[31:27]
     *   dword1: MaxExitLatency[15:0], RootHubPortNumber[23:16], NumberOfPorts[31:24]
     *   dword2: ParentHubSlotID[7:0], ParentPortNumber[15:8], TTThinkTime[17:16],
     *           InterrupterTarget[31:22]
     *   dword3: DeviceAddress[7:0], SlotState[31:27] (read-only)
     *
     * Previously wrote RootHubPortNumber to dword2[23:16] (wrong) instead
     * of dword1[23:16] — controller saw RootHubPortNumber=0 and treated
     * port_num as a Parent Hub address, returning cc=5 TRB Error. */
    {
        uint8_t *sc = ictx + XHCI_CTX_ENTRY_SIZE;
        ((volatile uint32_t *)sc)[0] =
            ((uint32_t)speed << 20) | (1u << 27);   /* speed + ContextEntries=1 */
        ((volatile uint32_t *)sc)[1] =
            (uint32_t)port_num << 16;               /* RootHubPortNumber */
    }

    /* EP0 Context at offset 2 * XHCI_CTX_ENTRY_SIZE.
     *
     * Endpoint Context layout (xHCI 1.0 §6.2.3):
     *   dword0: EPState[2:0] / Mult / MaxPStreams / LSA / Interval / MaxESITPayloadHi
     *   dword1: CErr[2:1], EPType[5:3], HID[7], MaxBurstSize[15:8], MaxPacketSize[31:16]
     *   dword2: DCS[0], reserved[3:1], TR Dequeue Pointer Lo[31:4]
     *   dword3: TR Dequeue Pointer Hi[31:0]
     *   dword4: AverageTRBLength[15:0], MaxESITPayloadLo[31:16]
     *
     * MaxPacketSize0 is speed-dependent per xHCI spec §4.3.4:
     *   LS=8, FS=8 (later updated), HS=64, SS=512.
     *
     * The previous version set ONLY dword1, leaving the TR Dequeue
     * Pointer at zero — Address Device "succeeded" structurally but
     * subsequent control transfers timed out because the controller
     * had no transfer ring to read commands from. */
    {
        uint16_t max_packet_size_0;
        uint64_t tr_phys;
        switch (speed) {
        case XHCI_SPEED_LS: max_packet_size_0 = 8;   break;
        case XHCI_SPEED_FS: max_packet_size_0 = 8;   break;
        case XHCI_SPEED_HS: max_packet_size_0 = 64;  break;
        case XHCI_SPEED_SS: max_packet_size_0 = 512; break;
        default:            max_packet_size_0 = 8;   break;
        }
        tr_phys = s_xfer_ring_phys[slot_id];

        uint8_t *ep0 = ictx + 2u * XHCI_CTX_ENTRY_SIZE;
        /* dword1: CErr=3, EPType=Control(4), MaxPacketSize */
        ((volatile uint32_t *)ep0)[1] =
            (3u << 1) | (XHCI_EP_TYPE_CTRL << 3) |
            ((uint32_t)max_packet_size_0 << 16);
        /* dword2: TR Dequeue Pointer Lo | DCS (= initial cycle = 1) */
        ((volatile uint32_t *)ep0)[2] =
            (uint32_t)(tr_phys & 0xFFFFFFF0u) |
            (uint32_t)s_xfer_cycle[slot_id];
        /* dword3: TR Dequeue Pointer Hi */
        ((volatile uint32_t *)ep0)[3] = (uint32_t)(tr_phys >> 32);
        /* dword4: Average TRB Length = 8 (control transfers are small) */
        ((volatile uint32_t *)ep0)[4] = 8u;
    }

    /* Clear DCBAA entry — Address Device will populate device context PA */
    s_dcbaa[slot_id] = 0;

    enqueue_cmd(ictx_phys, 0,
                (uint32_t)(XHCI_TRB_ADDRESS_DEVICE << 10) |
                ((uint32_t)slot_id << 24));
    ring_cmd_doorbell();
    if (poll_cmd_completion() == 0)
        return -1;
    return 0;
}

/* issue_configure_ep — add EP1 IN to the slot's device context.
 *
 * Input Context layout:
 *   [0]  Input Control Context  (32B): Add A0(slot) + A3(EP1IN DCI=3)
 *   [1]  Slot Context           (32B): Speed, ContextEntries=3, RootHubPort
 *   [2]  EP0 Context            (32B): zeroed (not re-adding EP0)
 *   [3]  EP1 OUT Context        (32B): zeroed (not used)
 *   [4]  EP1 IN  Context        (32B): EPType=INT_IN, MaxPktSize=8, TR ptr
 *
 * The EP1 IN context is at xHCI DCI=3, which maps to Input Context index 3
 * (after Input Control + Slot = indices 0/1 in the driver-local naming).
 * In bytes: offset = (DCI + 1) * 32 = 4 * 32 = 128.
 *
 * Returns 0 on success, -1 on failure. */
static int
issue_configure_ep(uint8_t slot_id, uint8_t port_num, uint8_t speed)
{
    uint64_t  ictx_phys;
    uint8_t  *ictx = (uint8_t *)alloc_page(&ictx_phys);
    uint64_t  xfer_phys = s_xfer_ring_phys[slot_id];

    /* Input Control Context: Add A0(slot bit0) + A3(EP1IN DCI=3, bit3) = 0x9 */
    ((volatile uint32_t *)ictx)[0] = 0u;
    ((volatile uint32_t *)ictx)[1] = 0x9u;

    /* Slot Context: ContextEntries=3 (highest DCI in use is 3 = EP1 IN).
     * RootHubPortNumber goes in dword1[23:16], NOT dword2 (same fix as
     * issue_address_device — see comment there). */
    {
        uint8_t *sc = ictx + XHCI_CTX_ENTRY_SIZE;
        ((volatile uint32_t *)sc)[0] =
            ((uint32_t)speed << 20) | (3u << 27);
        ((volatile uint32_t *)sc)[1] = (uint32_t)port_num << 16;
    }

    /* EP1 IN Context at byte offset (DCI+1) * ctx_size = 4 * ctx_size.
     *
     * Interval field encoding (xHCI 1.0 §6.2.3.6):
     *   LS/FS interrupt: bInterval direct (1-255 ms)
     *   HS interrupt:    2^(bInterval-1) × 125us
     * For our LS K120 (bInterval=10), Interval=10 (0xA) = 10ms polling.
     *
     * MaxPacketSize is set to 64 even though boot keyboards declare 8,
     * so the controller doesn't fire cc=3 (Babble) if the device sends
     * a slightly oversized packet. K120 in practice sends 8 but the
     * controller validates against the EP context value, not the
     * descriptor value. */
    {
        uint8_t *ep1in = ictx + 4u * XHCI_CTX_ENTRY_SIZE;
        /* dword0: Interval[23:16] */
        ((volatile uint32_t *)ep1in)[0] = (0xAu << 16);
        /* dword1: CErr=3, EPType=INT_IN(7), MaxPacketSize=64 */
        ((volatile uint32_t *)ep1in)[1] =
            (3u << 1) | (XHCI_EP_TYPE_INT_IN << 3) | (64u << 16);
        /* dword2: TR Dequeue Pointer Lo [63:4] | DCS (dequeue cycle state) */
        ((volatile uint32_t *)ep1in)[2] =
            (uint32_t)(xfer_phys & 0xFFFFFFF0u) |
            (uint32_t)s_xfer_cycle[slot_id];
        /* dword3: TR Dequeue Pointer Hi */
        ((volatile uint32_t *)ep1in)[3] = (uint32_t)(xfer_phys >> 32);
        /* dword4: Average TRB Length = 8 bytes */
        ((volatile uint32_t *)ep1in)[4] = 8u;
    }

    enqueue_cmd(ictx_phys, 0,
                (uint32_t)(XHCI_TRB_CONFIGURE_EP << 10) |
                ((uint32_t)slot_id << 24));
    ring_cmd_doorbell();
    if (poll_cmd_completion() == 0)
        return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Control transfer helpers for device enumeration
 * ---------------------------------------------------------------------- */

/* issue_control_transfer — send a SETUP + DATA(IN) + STATUS(OUT) sequence on
 * EP0 (DCI=1) for the given slot.  The 8-byte response lands in
 * s_hid_buf[slot_id].  Returns bytes transferred or -1 on error.
 *
 * setup_pkt: the 8-byte USB setup packet encoded as a uint64_t:
 *   bits [7:0]   = bmRequestType
 *   bits [15:8]  = bRequest
 *   bits [31:16] = wValue
 *   bits [47:32] = wIndex
 *   bits [63:48] = wLength
 *
 * For a no-data transfer (wLength==0), pass only SETUP + STATUS (no DATA
 * stage); the STATUS direction is IN for host-to-device transfers and OUT
 * for device-to-host transfers (opposite of data direction). */
static int
issue_control_transfer(uint8_t slot_id, uint64_t setup_pkt, int has_data_in)
{
    xhci_trb_t *ring  = s_xfer_ring[slot_id];
    uint32_t    cycle = (uint32_t)s_xfer_cycle[slot_id];
    uint32_t    ei    = s_xfer_enqueue[slot_id];
    uint16_t    wlen  = (uint16_t)(setup_pkt >> 48);
    volatile uint32_t *db;
    volatile xhci_trb_t *evt;
    uint32_t timeout;

    if (!ring || !s_hid_buf[slot_id])
        return -1;

    /* --- Setup TRB: type=2, IDT=1(bit6), TRT=3(IN, bits17:16), len=8 --- */
    {
        xhci_trb_t *trb = &ring[ei];
        trb->param   = setup_pkt;
        trb->status  = 8u;                                /* setup packet length */
        trb->control = (uint32_t)(XHCI_TRB_SETUP << 10)  /* type */
                     | (1u << 6)                           /* IDT (Immediate Data) */
                     | (has_data_in ? (3u << 16) : 0u)     /* TRT: 3=IN, 0=No Data */
                     | cycle;
        ei++;
        if (ei >= XHCI_TRANSFER_RING_SIZE - 1u) {
            s_xfer_cycle[slot_id] ^= 1;
            ei = 0;
            ring[XHCI_TRANSFER_RING_SIZE - 1].control =
                (uint32_t)(XHCI_TRB_LINK << 10) | (1u << 1) |
                (uint32_t)s_xfer_cycle[slot_id];
            cycle = (uint32_t)s_xfer_cycle[slot_id];
        }
    }

    /* --- Data TRB (only if wLength > 0) --- */
    if (has_data_in && wlen > 0) {
        xhci_trb_t *trb = &ring[ei];
        trb->param   = s_hid_buf_phys[slot_id];
        trb->status  = (uint32_t)wlen;
        trb->control = (uint32_t)(XHCI_TRB_DATA << 10)
                     | (1u << 16)                          /* DIR=1 (IN) */
                     | cycle;
        ei++;
        if (ei >= XHCI_TRANSFER_RING_SIZE - 1u) {
            s_xfer_cycle[slot_id] ^= 1;
            ei = 0;
            ring[XHCI_TRANSFER_RING_SIZE - 1].control =
                (uint32_t)(XHCI_TRB_LINK << 10) | (1u << 1) |
                (uint32_t)s_xfer_cycle[slot_id];
            cycle = (uint32_t)s_xfer_cycle[slot_id];
        }
    }

    /* --- Status TRB --- */
    {
        xhci_trb_t *trb = &ring[ei];
        trb->param   = 0;
        trb->status  = 0;
        /* Status direction is opposite of data direction:
         *   IN data  → OUT status (DIR=0)
         *   No data (host-to-device setup) → IN status (DIR=1) */
        trb->control = (uint32_t)(XHCI_TRB_STATUS << 10)
                     | (1u << 5)                           /* IOC */
                     | (has_data_in ? 0u : (1u << 16))     /* DIR: 0=OUT, 1=IN */
                     | cycle;
        ei++;
        if (ei >= XHCI_TRANSFER_RING_SIZE - 1u) {
            s_xfer_cycle[slot_id] ^= 1;
            ei = 0;
            ring[XHCI_TRANSFER_RING_SIZE - 1].control =
                (uint32_t)(XHCI_TRB_LINK << 10) | (1u << 1) |
                (uint32_t)s_xfer_cycle[slot_id];
            cycle = (uint32_t)s_xfer_cycle[slot_id];
        }
    }

    s_xfer_enqueue[slot_id] = ei;

    /* Ring doorbell for EP0 (DCI=1) */
    db = (volatile uint32_t *)(s_bar0_va + s_cap->dboff);
    arch_wmb();
    db[slot_id] = 1u;   /* DCI=1 for EP0 */

    /* Poll for Transfer Event completion. Skip non-transfer events
     * (port status changes, leftover CMD_COMPLETIONs from earlier
     * commands, etc.) instead of bailing — same fix as
     * poll_cmd_completion's PSC skip. */
    timeout = 2000000u;
    while (timeout--) {
        evt = &s_evt_ring[s_evt_dequeue];
        if ((evt->control & 1u) != (uint32_t)s_evt_cycle)
            continue;

        {
            uint32_t ctrl     = evt->control;
            uint32_t etype    = (ctrl >> 10) & 0x3Fu;
            uint8_t  cc       = (uint8_t)((evt->status >> 24) & 0xFFu);
            uint32_t residual = evt->status & 0xFFFFFFu;

            s_evt_dequeue++;
            if (s_evt_dequeue >= XHCI_EVT_RING_SIZE) {
                s_evt_dequeue = 0;
                s_evt_cycle  ^= 1;
            }
            update_erdp();

            if (etype == XHCI_TRB_TRANSFER_EVENT) {
                if (cc == 1u || cc == 13u) {
                    /* cc=1: Success, cc=13: Short Packet (normal) */
                    return (int)((uint32_t)wlen - residual);
                }
                printk("[XHCI] ctrl_xfer transfer event cc=%u\n",
                       (unsigned)cc);
                return -1;
            }
            /* Other event types (PSC, leftover CMD_COMPLETION) — skip */
            printk("[XHCI] ctrl_xfer skip etype=%u\n", (unsigned)etype);
        }
    }
    printk("[XHCI] ctrl_xfer TIMEOUT\n");
    return -1;
}

/* detect_hid_protocol — issue GET_DESCRIPTOR(Configuration) and walk the
 * returned descriptor chain for an Interface Descriptor (type=4) with
 * bInterfaceClass=3 (HID).  Returns bInterfaceProtocol:
 *   1 = keyboard, 2 = mouse, 0 = unknown/not HID. */
static uint8_t
detect_hid_protocol(uint8_t slot_id)
{
    /* GET_DESCRIPTOR: bmRequestType=0x80 (Device-to-Host, Standard, Device),
     * bRequest=6 (GET_DESCRIPTOR), wValue=0x0200 (Configuration, index 0),
     * wIndex=0, wLength=64 */
    uint64_t setup = (uint64_t)0x80u          /* bmRequestType */
                   | ((uint64_t)0x06u << 8)   /* bRequest */
                   | ((uint64_t)0x0200u << 16) /* wValue: Config desc, idx 0 */
                   | ((uint64_t)0u << 32)      /* wIndex */
                   | ((uint64_t)64u << 48);    /* wLength */
    int got;
    uint8_t *buf;
    int off;

    got = issue_control_transfer(slot_id, setup, 1);
    printk("[XHCI] slot %u GET_DESCRIPTOR(Config) got=0x%x\n",
           (unsigned)slot_id, (unsigned)got);
    if (got < 4)
        return 0;

    buf = s_hid_buf[slot_id];
    /* Dump 36 bytes of config descriptor — enough to see config + iface 0
     * + HID descriptor + EP1 IN endpoint descriptor for the boot kbd. */
    printk("[XHCI] cfg desc 0-15: %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n",
           (unsigned)buf[0],  (unsigned)buf[1],  (unsigned)buf[2],  (unsigned)buf[3],
           (unsigned)buf[4],  (unsigned)buf[5],  (unsigned)buf[6],  (unsigned)buf[7],
           (unsigned)buf[8],  (unsigned)buf[9],  (unsigned)buf[10], (unsigned)buf[11],
           (unsigned)buf[12], (unsigned)buf[13], (unsigned)buf[14], (unsigned)buf[15]);
    printk("[XHCI] cfg desc 16-31: %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n",
           (unsigned)buf[16], (unsigned)buf[17], (unsigned)buf[18], (unsigned)buf[19],
           (unsigned)buf[20], (unsigned)buf[21], (unsigned)buf[22], (unsigned)buf[23],
           (unsigned)buf[24], (unsigned)buf[25], (unsigned)buf[26], (unsigned)buf[27],
           (unsigned)buf[28], (unsigned)buf[29], (unsigned)buf[30], (unsigned)buf[31]);

    /* Walk the descriptor chain */
    off = 0;
    while (off + 2 <= got) {
        uint8_t blen  = buf[off];
        uint8_t btype = buf[off + 1];

        if (blen < 2 || off + blen > got)
            break;

        /* Interface Descriptor: bDescriptorType=4, bLength>=9 */
        if (btype == 4u && blen >= 9u) {
            uint8_t bclass = buf[off + 5];   /* bInterfaceClass */
            uint8_t bproto = buf[off + 7];   /* bInterfaceProtocol */
            if (bclass == 3u)                /* HID class */
                return bproto;               /* 1=kbd, 2=mouse */
        }
        off += blen;
    }
    return 0;
}

/* issue_set_protocol — send SET_PROTOCOL(Boot Protocol=0) class request.
 * bmRequestType=0x21 (Host-to-Device, Class, Interface),
 * bRequest=0x0B (SET_PROTOCOL), wValue=0 (Boot Protocol),
 * wIndex=0, wLength=0.  No data stage. */
static void
issue_set_protocol(uint8_t slot_id)
{
    uint64_t setup = (uint64_t)0x21u          /* bmRequestType */
                   | ((uint64_t)0x0Bu << 8)   /* bRequest: SET_PROTOCOL */
                   | ((uint64_t)0u << 16)      /* wValue: 0 = Boot Protocol */
                   | ((uint64_t)0u << 32)      /* wIndex */
                   | ((uint64_t)0u << 48);     /* wLength */
    issue_control_transfer(slot_id, setup, 0);
}

/* enumerate_port — detect device on one port (1-based), reset it, run
 * Enable Slot + Address Device + Configure EP, detect HID type, then
 * schedule first interrupt IN transfer if successful. */
static void
enumerate_port(uint32_t port_num)
{
    /* Port register array: op_base + 0x400 + (port_num-1)*16 */
    uint8_t *op_base = (uint8_t *)s_op;
    /* SAFETY: The port register offset is within the 64KB BAR0 mapping. */
    volatile uint32_t *portsc_reg =
        (volatile uint32_t *)(op_base + 0x400u + (port_num - 1u) * 16u);

    uint32_t portsc;
    uint8_t  slot_id;
    uint8_t  speed;
    uint32_t t;

    portsc = *portsc_reg;
    printk("[XHCI] enum port %u portsc=0x%x ccs=%u pr=%u prc=%u\n",
           (unsigned)port_num, (unsigned)portsc,
           (portsc & XHCI_PORTSC_CCS) ? 1u : 0u,
           (portsc & XHCI_PORTSC_PR)  ? 1u : 0u,
           (portsc & XHCI_PORTSC_PRC) ? 1u : 0u);
    if (!(portsc & XHCI_PORTSC_CCS))
        return;   /* nothing connected */

    /* Extract speed from PORTSC[13:10] */
    speed = (uint8_t)((portsc >> 10) & 0xFu);
    if (speed == 0)
        speed = XHCI_SPEED_HS;
    printk("[XHCI] port %u speed=%u (1=FS,2=LS,3=HS,4=SS)\n",
           (unsigned)port_num, (unsigned)speed);

    /* Port Reset: set PR bit (bit 4), clear PRC (RW1C bit 21 — writing 1
     * to other RW1C bits must be avoided; preserve the port value and only
     * set PR while writing 0 to all RW1C fields except what we want). */
    *portsc_reg = (portsc & ~(XHCI_PORTSC_PRC)) | XHCI_PORTSC_PR;

    /* Wait for PRC (Port Reset Change) to set */
    t = 2000000u;
    while (t--) {
        if (*portsc_reg & XHCI_PORTSC_PRC)
            break;
    }
    if (!(*portsc_reg & XHCI_PORTSC_PRC)) {
        if (!s_post_boot)
            printk("[XHCI] port %u: reset timeout\n", (unsigned)port_num);
        return;
    }
    /* Clear PRC by writing 1 to it (RW1C) */
    *portsc_reg = (*portsc_reg & ~0u) | XHCI_PORTSC_PRC;

    /* Pre-Enable-Slot diagnostic dump */
    {
        uint32_t usbsts = op_read32(XHCI_OP_USBSTS_OFF);
        uint32_t usbcmd = op_read32(XHCI_OP_USBCMD_OFF);
        printk("[XHCI] before EnableSlot: usbcmd=0x%x usbsts=0x%x s_cmd_enq=%u s_cmd_cyc=%u s_evt_deq=%u s_evt_cyc=%u\n",
               (unsigned)usbcmd, (unsigned)usbsts,
               (unsigned)s_cmd_enqueue, (unsigned)s_cmd_cycle,
               (unsigned)s_evt_dequeue, (unsigned)s_evt_cycle);
    }

    /* Enable Slot */
    slot_id = issue_enable_slot();

    /* Post-Enable-Slot diagnostic dump */
    {
        uint32_t usbsts = op_read32(XHCI_OP_USBSTS_OFF);
        printk("[XHCI] after EnableSlot: slot_id=%u usbsts=0x%x evt_ring[0].ctrl=0x%x evt_ring[0].status=0x%x\n",
               (unsigned)slot_id, (unsigned)usbsts,
               (unsigned)s_evt_ring[0].control,
               (unsigned)s_evt_ring[0].status);
    }

    if (slot_id == 0 || slot_id >= XHCI_MAX_SLOTS) {
        if (!s_post_boot)
            printk("[XHCI] port %u: Enable Slot failed\n", (unsigned)port_num);
        return;
    }

    /* Allocate transfer ring for this slot */
    {
        xhci_trb_t *xr =
            (xhci_trb_t *)alloc_page(&s_xfer_ring_phys[slot_id]);
        s_xfer_ring[slot_id]    = xr;
        s_xfer_enqueue[slot_id] = 0;
        s_xfer_cycle[slot_id]   = 1;
        /* Link TRB at end — wraps back to start, toggle-cycle=1 */
        xr[XHCI_TRANSFER_RING_SIZE - 1].param   = s_xfer_ring_phys[slot_id];
        xr[XHCI_TRANSFER_RING_SIZE - 1].status  = 0;
        xr[XHCI_TRANSFER_RING_SIZE - 1].control =
            (uint32_t)(XHCI_TRB_LINK << 10) | (1u << 1) | 1u;
    }

    /* Allocate 8-byte HID boot report buffer */
    s_hid_buf[slot_id] =
        (uint8_t *)alloc_page(&s_hid_buf_phys[slot_id]);

    /* Address Device */
    if (issue_address_device(slot_id, (uint8_t)port_num, speed) != 0) {
        if (!s_post_boot)
            printk("[XHCI] port %u: Address Device failed\n",
                   (unsigned)port_num);
        return;
    }
    printk("[XHCI] slot %u Address Device OK\n", (unsigned)slot_id);

    /* Configure EP1 IN */
    if (issue_configure_ep(slot_id, (uint8_t)port_num, speed) != 0) {
        if (!s_post_boot)
            printk("[XHCI] port %u: Configure Endpoint failed\n",
                   (unsigned)port_num);
        return;
    }
    printk("[XHCI] slot %u Configure EP OK\n", (unsigned)slot_id);

    /* Detect HID device type via Configuration Descriptor */
    {
        uint8_t proto = detect_hid_protocol(slot_id);
        printk("[XHCI] slot %u detect_hid_protocol returned %u\n",
               (unsigned)slot_id, (unsigned)proto);

        if (proto == 1) {
            issue_set_protocol(slot_id);
            s_hid_slots[slot_id]     = 1;
            s_hid_slot_type[slot_id] = USB_DEV_KBD;
            s_slot_port[slot_id]     = (uint8_t)port_num;
        } else if (proto == 2) {
            issue_set_protocol(slot_id);
            s_hid_slots[slot_id]     = 1;
            s_hid_slot_type[slot_id] = USB_DEV_MOUSE;
            s_slot_port[slot_id]     = (uint8_t)port_num;
        } else {
            return;
        }
    }

    /* Schedule the first interrupt IN transfer.
     * Note: we only queue ONE TRB and re-arm after each completion.
     * This is fine for keyboards (typing rate << polling rate). If
     * report drops are observed under load, increase the queue depth. */
    xhci_schedule_interrupt_in(slot_id, XHCI_EP1_IN_DCI,
                               s_hid_buf_phys[slot_id], 8u);
    printk("[XHCI] slot %u HID kbd ready, first interrupt-in scheduled\n",
           (unsigned)slot_id);
}

/* -------------------------------------------------------------------------
 * xhci_init
 * ---------------------------------------------------------------------- */

void
xhci_init(void)
{
    uint32_t i;

    /* Step 1: Locate xHCI controller via PCIe.
     * class=0x0C (Serial Bus), subclass=0x03 (USB), prog-if=0x30 (xHCI). */
    const pcie_device_t *dev = pcie_find_device(0x0C, 0x03, 0x30);
    if (dev == NULL)
        return;   /* no xHCI device — silent skip, make test stays GREEN */

    /* Step 2: Map BAR0 MMIO.
     * pcie_device_t stores decoded 64-bit base addresses in bar[].
     * pcie.c strips the flag bits during enumeration (same as nvme.c).
     * Map with WC+UCMINUS (uncached MMIO). */
    {
        uint64_t  bar0_phys = dev->bar[0];
        uintptr_t bar0_kva  = (uintptr_t)kva_alloc_pages(XHCI_BAR0_PAGES);

        for (i = 0; i < XHCI_BAR0_PAGES; i++) {
            uintptr_t va = bar0_kva + (uintptr_t)i * 4096u;
            /* Unmap the PMM-backed page kva_alloc_pages installed so that
             * vmm_map_page does not panic on a double-map.
             * SAFETY: va is a present kva page; vmm_unmap_page is valid. */
            vmm_unmap_page(va);
            /* SAFETY: map BAR0 MMIO uncached (WC+UCMINUS = PWT+PCD).
             * The PA is device MMIO; the kernel VA is the intended accessor. */
            vmm_map_page(va, bar0_phys + (uint64_t)i * 4096u,
                         VMM_FLAG_WRITABLE | VMM_FLAG_WC | VMM_FLAG_UCMINUS);
        }
        s_bar0_va = (uint8_t *)bar0_kva;
    }

    /* SAFETY: s_cap/s_op are volatile casts of MMIO-mapped kernel VAs. */
    s_cap = (volatile xhci_cap_regs_t *)s_bar0_va;
    s_op  = (volatile xhci_op_regs_t  *)(s_bar0_va + s_cap->caplength);

    s_max_slots = (s_cap->hcsparams1)       & 0xFFu;
    s_max_ports = (s_cap->hcsparams1 >> 24) & 0xFFu;

    /* HCCPARAMS1.CSZ (bit 2) — 0 = 32-byte contexts, 1 = 64-byte. */
    s_ctx_entry_size = (s_cap->hccparams1 & (1u << 2)) ? 64u : 32u;
    printk("[XHCI] hccparams1=0x%x ctx_size=%u\n",
           (unsigned)s_cap->hccparams1, (unsigned)s_ctx_entry_size);

    /* Step 3: Stop controller — clear USBCMD.RS, wait for USBSTS.HCH.
     * Use op_read32/op_write32 to avoid packed-member address errors. */
    op_write32(XHCI_OP_USBCMD_OFF,
               op_read32(XHCI_OP_USBCMD_OFF) & ~XHCI_CMD_RS);
    if (op_spin_until_set(XHCI_OP_USBSTS_OFF, XHCI_STS_HCH) != 0) {
        printk("[XHCI] FAIL: controller did not stop\n");
        return;
    }

    printk("[XHCI] pre-HCRST usbsts=0x%x usbcmd=0x%x\n",
           (unsigned)op_read32(XHCI_OP_USBSTS_OFF),
           (unsigned)op_read32(XHCI_OP_USBCMD_OFF));

    /* Step 4: Reset controller — set USBCMD.HCRST, wait for it to clear. */
    op_write32(XHCI_OP_USBCMD_OFF,
               op_read32(XHCI_OP_USBCMD_OFF) | XHCI_CMD_HCRST);
    if (op_spin_until_clear(XHCI_OP_USBCMD_OFF, XHCI_CMD_HCRST) != 0) {
        printk("[XHCI] FAIL: controller reset timeout\n");
        return;
    }
    printk("[XHCI] post-HCRST usbsts=0x%x\n",
           (unsigned)op_read32(XHCI_OP_USBSTS_OFF));

    /* xHCI spec §4.2: software shall not write any operational/runtime
     * register (other than USBSTS) until USBSTS.CNR is '0'. */
    if (op_spin_until_clear(XHCI_OP_USBSTS_OFF, XHCI_STS_CNR) != 0) {
        printk("[XHCI] FAIL: CNR did not clear after reset\n");
        return;
    }
    printk("[XHCI] post-CNR-clear usbsts=0x%x\n",
           (unsigned)op_read32(XHCI_OP_USBSTS_OFF));

    /* Clear any sticky RW1C bits in USBSTS so we start with a clean slate. */
    op_write32(XHCI_OP_USBSTS_OFF,
               XHCI_STS_HSE | XHCI_STS_EINT | XHCI_STS_HCE);
    printk("[XHCI] post-RW1C-clear usbsts=0x%x\n",
           (unsigned)op_read32(XHCI_OP_USBSTS_OFF));

    /* Step 5: Allocate DCBAA (Device Context Base Address Array).
     * DCBAA[0] is reserved (must be 0); slots are 1..MaxSlots. */
    {
        uint8_t *dcbaa_va = (uint8_t *)alloc_page(&s_dcbaa_phys);
        s_dcbaa      = (uint64_t *)dcbaa_va;
        /* SAFETY: s_op->dcbaap is a packed MMIO field at a known fixed offset.
         * Writing through op_write32 would require a 64-bit accessor; the
         * dcbaap field is at offset 0x30 in xhci_op_regs_t.  Access it via
         * a volatile 64-bit pointer derived from the base address. */
        {
            volatile uint64_t *dcbaap =
                (volatile uint64_t *)((volatile uint8_t *)s_op + 0x30u);
            *dcbaap = s_dcbaa_phys;
        }
    }

    /* Step 6: Command Ring — alloc one page, place Link TRB at index 63,
     * write CRCR with PA | RCS (Running Cycle State = initial cycle bit). */
    {
        s_cmd_ring    = (xhci_trb_t *)alloc_page(&s_cmd_ring_phys);
        s_cmd_enqueue = 0;
        s_cmd_cycle   = 1;

        s_cmd_ring[XHCI_CMD_RING_SIZE - 1].param   = s_cmd_ring_phys;
        s_cmd_ring[XHCI_CMD_RING_SIZE - 1].status  = 0;
        s_cmd_ring[XHCI_CMD_RING_SIZE - 1].control =
            (uint32_t)(XHCI_TRB_LINK << 10) | (1u << 1) | 1u;

        /* CRCR at offset 0x18 in xhci_op_regs_t */
        {
            volatile uint64_t *crcr =
                (volatile uint64_t *)((volatile uint8_t *)s_op + 0x18u);
            *crcr = s_cmd_ring_phys | 1u;   /* PA | RCS=1 */
        }
    }

    /* Steps 7+8+9: Event Ring Segment + ERST + Interrupter 0 registers */
    {
        uint64_t              evt_ring_phys;
        volatile xhci_trb_t  *evt_ring;
        uint64_t              erst_phys;
        uint64_t             *erst;
        uint8_t              *rts;

        evt_ring      = (volatile xhci_trb_t *)alloc_page(&evt_ring_phys);
        s_evt_ring    = evt_ring;
        s_evt_dequeue = 0;
        s_evt_cycle   = 1;

        /* ERST entry (16 bytes):
         *   [0..7]:   Ring Segment Base Address (64-bit PA)
         *   [8..11]:  Ring Segment Size (TRB count, low 32 bits)
         *   [12..15]: Reserved — zeroed by alloc_page */
        erst    = (uint64_t *)alloc_page(&erst_phys);
        erst[0] = evt_ring_phys;
        erst[1] = XHCI_EVT_RING_SIZE;   /* low 32-bit count; high 32=0 */

        /* Interrupter 0 register set (xHCI 1.0 §5.5.2):
         *   IMAN   at +0x00 (32-bit) — Interrupter Management
         *   IMOD   at +0x04 (32-bit) — Interrupter Moderation
         *   ERSTSZ at +0x08 (32-bit) — Event Ring Segment Table Size
         *   (reserved at +0x0C)
         *   ERSTBA at +0x10 (64-bit) — Event Ring Segment Table Base Address
         *   ERDP   at +0x18 (64-bit) — Event Ring Dequeue Pointer
         *
         * The interrupter registers begin at runtime_base + 0x20 (the first
         * 0x20 bytes are MFINDEX + reserved). */
        rts = s_bar0_va + s_cap->rtsoff;
        {
            volatile uint32_t *erstsz =
                (volatile uint32_t *)(rts + 0x20u + 0x08u);
            volatile uint64_t *erstba =
                (volatile uint64_t *)(rts + 0x20u + 0x10u);
            volatile uint64_t *erdp =
                (volatile uint64_t *)(rts + 0x20u + 0x18u);

            *erstsz = 1u;
            *erstba = erst_phys;
            *erdp   = evt_ring_phys;
        }
    }

    /* Step 10: Configure MaxSlotsEn (CONFIG register at offset 0x38) */
    {
        volatile uint32_t *config_reg =
            (volatile uint32_t *)((volatile uint8_t *)s_op + 0x38u);
        *config_reg = s_max_slots;
    }

    /* Step 11: Start controller — set USBCMD.RS, wait for HCH to clear. */
    op_write32(XHCI_OP_USBCMD_OFF,
               op_read32(XHCI_OP_USBCMD_OFF) | XHCI_CMD_RS);
    if (op_spin_until_clear(XHCI_OP_USBSTS_OFF, XHCI_STS_HCH) != 0) {
        printk("[XHCI] FAIL: controller did not start\n");
        return;
    }
    {
        uint32_t usbsts = op_read32(XHCI_OP_USBSTS_OFF);
        uint32_t usbcmd = op_read32(XHCI_OP_USBCMD_OFF);
        volatile uint64_t *crcr =
            (volatile uint64_t *)((volatile uint8_t *)s_op + 0x18u);
        printk("[XHCI] running: usbcmd=0x%x usbsts=0x%x crcr=0x%x cmd_ring_phys=0x%x\n",
               (unsigned)usbcmd, (unsigned)usbsts,
               (unsigned)*crcr, (unsigned)s_cmd_ring_phys);
    }

    /* Step 12: Enumerate ports */
    for (i = 1; i <= s_max_ports && i < XHCI_MAX_SLOTS; i++)
        enumerate_port(i);

    s_post_boot   = 1;
    s_xhci_active = 1;

    printk("[XHCI] OK: %u ports, %u slots\n",
           (unsigned)s_max_ports, (unsigned)s_max_slots);
}

/* -------------------------------------------------------------------------
 * xhci_poll — called from PIT ISR at 100 Hz
 * ---------------------------------------------------------------------- */

void
xhci_poll(void)
{
    volatile xhci_trb_t *trb;

    if (!s_xhci_active)
        return;

    trb = &s_evt_ring[s_evt_dequeue];

    /* SAFETY: trb is a volatile pointer into a kva-mapped page; the hardware
     * writes the cycle bit when a TRB is posted.  Volatile prevents the
     * compiler from hoisting the load out of the while loop. */
    while ((trb->control & 1u) == (uint32_t)s_evt_cycle) {
        uint32_t ctrl     = trb->control;
        uint32_t trb_type = (ctrl >> 10) & 0x3Fu;
        uint8_t  slot     = (uint8_t)((ctrl >> 24) & 0xFFu);

        if (trb_type == XHCI_TRB_TRANSFER_EVENT &&
            slot > 0 && slot < XHCI_MAX_SLOTS && s_hid_slots[slot]) {
            uint8_t cc = (uint8_t)((trb->status >> 24) & 0xFFu);
            uint32_t residual = trb->status & 0xFFFFFFu;
            if (!s_hid_buf[slot]) goto next_trb;  /* alloc failure guard */
            uint8_t *b = s_hid_buf[slot];
            printk("[XHCI] xfer evt slot=%u cc=%u resid=%u\n",
                   (unsigned)slot, (unsigned)cc, (unsigned)residual);
            printk("[XHCI]  buf 0-15: %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n",
                   (unsigned)b[0],  (unsigned)b[1],  (unsigned)b[2],  (unsigned)b[3],
                   (unsigned)b[4],  (unsigned)b[5],  (unsigned)b[6],  (unsigned)b[7],
                   (unsigned)b[8],  (unsigned)b[9],  (unsigned)b[10], (unsigned)b[11],
                   (unsigned)b[12], (unsigned)b[13], (unsigned)b[14], (unsigned)b[15]);
            if (s_hid_slot_type[slot] == USB_DEV_KBD)
                usb_hid_process_report(s_hid_buf[slot], 8u);
            else if (s_hid_slot_type[slot] == USB_DEV_MOUSE)
                usb_mouse_process_report(s_hid_buf[slot], 8u);
            /* Re-arm: schedule the next interrupt IN */
            xhci_schedule_interrupt_in(slot, XHCI_EP1_IN_DCI,
                                       s_hid_buf_phys[slot], 8u);
        }

        if (trb_type == XHCI_TRB_PORT_STATUS_CHG) {
            /* Port ID is in bits [31:24] of param (low dword).
             * xHCI spec §6.4.2.3: Port Status Change Event TRB
             * param[31:24] = Port ID (1-based). */
            uint8_t port_id = (uint8_t)((trb->param >> 24) & 0xFFu);
            if (port_id >= 1 && port_id <= s_max_ports) {
                volatile uint32_t *portsc_reg =
                    (volatile uint32_t *)((volatile uint8_t *)s_op + 0x400u +
                                          (port_id - 1u) * 16u);
                uint32_t portsc = *portsc_reg;

                if (portsc & XHCI_PORTSC_CSC) {
                    /* Clear CSC (RW1C bit 17). Write 1 to CSC while preserving
                     * non-RW1C bits and not accidentally clearing other RW1C
                     * bits. PORTSC RW1C bits: CSC(17), PEC(18), WRC(19),
                     * OCC(20), PRC(21), PLC(22), CEC(23).
                     * Mask = 0x00FE0000. Write 1 only to CSC. */
                    *portsc_reg = (portsc & ~0x00FE0000u) | XHCI_PORTSC_CSC;

                    if (portsc & XHCI_PORTSC_CCS) {
                        /* Device connected — enumerate silently (s_post_boot=1) */
                        enumerate_port(port_id);
                    } else {
                        /* Device disconnected — deactivate slot */
                        uint32_t s;
                        for (s = 1; s < XHCI_MAX_SLOTS; s++) {
                            if (s_slot_port[s] == port_id && s_hid_slots[s]) {
                                s_hid_slots[s]     = 0;
                                s_hid_slot_type[s]  = USB_DEV_NONE;
                                s_slot_port[s]      = 0;
                                break;
                            }
                        }
                    }
                }
            }
        }

        next_trb:
        s_evt_dequeue++;
        if (s_evt_dequeue >= XHCI_EVT_RING_SIZE) {
            s_evt_dequeue = 0;
            s_evt_cycle  ^= 1;
        }
        update_erdp();

        trb = &s_evt_ring[s_evt_dequeue];
    }
}

/* -------------------------------------------------------------------------
 * xhci_schedule_interrupt_in — place a Normal TRB on a slot's transfer ring
 * ---------------------------------------------------------------------- */

int
xhci_schedule_interrupt_in(uint8_t slot_id, uint8_t ep_id,
                            uint64_t buf_phys, uint32_t buf_len)
{
    xhci_trb_t        *trb;
    volatile uint32_t *db;

    if (slot_id == 0 || slot_id >= XHCI_MAX_SLOTS ||
        s_xfer_ring[slot_id] == NULL)
        return -1;

    trb = &s_xfer_ring[slot_id][s_xfer_enqueue[slot_id]];
    trb->param  = buf_phys;
    trb->status = buf_len;
    /* Normal TRB | IOC (Interrupt On Completion, bit 5) | cycle bit */
    trb->control = (uint32_t)(XHCI_TRB_NORMAL << 10) |
                   (1u << 5) |
                   (uint32_t)s_xfer_cycle[slot_id];

    s_xfer_enqueue[slot_id]++;
    if (s_xfer_enqueue[slot_id] >= XHCI_TRANSFER_RING_SIZE - 1u) {
        /* Wrap: flip cycle, reset enqueue, update Link TRB cycle bit. */
        s_xfer_cycle[slot_id]  ^= 1;
        s_xfer_enqueue[slot_id] = 0;
        s_xfer_ring[slot_id][XHCI_TRANSFER_RING_SIZE - 1].control =
            (uint32_t)(XHCI_TRB_LINK << 10) | (1u << 1) |
            (uint32_t)s_xfer_cycle[slot_id];
    }

    /* Ring doorbell: slot_id selects device doorbell, ep_id selects endpoint.
     * SAFETY: s_bar0_va + cap->dboff is the doorbell array base within the
     * 64KB BAR0 mapping; db[slot_id] is within that range for slot_id < 32. */
    db = (volatile uint32_t *)(s_bar0_va + s_cap->dboff);
    /* sfence: TRB write must be globally visible before doorbell write. */
    arch_wmb();
    db[slot_id] = ep_id;

    return 0;
}
