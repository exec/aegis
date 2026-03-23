# Phase 22: xHCI + USB HID Keyboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement an xHCI USB host controller driver and a USB HID boot-protocol keyboard driver, enabling keyboard input on real hardware that lacks PS/2.

**Architecture:** The xHCI driver (`kernel/drivers/xhci.c`) discovers the controller via `pcie_find_device(0x0C, 0x03, 0x30)`, maps BAR0, initializes the controller (DCBAA, Command Ring, Event Ring), enumerates connected devices via port reset and Enable Slot / Address Device / Get Descriptor sequences, and identifies HID boot-protocol keyboards. The USB HID driver (`kernel/drivers/usb_hid.c`) parses 8-byte boot-protocol reports, translates HID usage IDs to ASCII, and injects keystrokes into the existing PS/2 keyboard ring buffer via `kbd_usb_inject()`. Polling is done from the PIT handler at 100Hz.

**Tech Stack:** C, xHCI 1.2 spec (TRB rings, device contexts, port management), USB HID boot protocol, PCIe BAR0 MMIO, existing PS/2 keyboard ring buffer.

**Spec:** docs/superpowers/specs/2026-03-23-aegis-v1-design.md — Phase 22

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/drivers/xhci.h` | Create | xHCI register structures, TRB types, ring management, `xhci_init()`, `xhci_poll()` |
| `kernel/drivers/xhci.c` | Create | Controller init, DCBAA, Command/Event rings, port reset, device enumeration |
| `kernel/drivers/usb_hid.h` | Create | HID boot report structure, `usb_hid_init()`, `usb_hid_process_report()` |
| `kernel/drivers/usb_hid.c` | Create | HID usage → ASCII table, report parsing, `kbd_usb_inject()` calls |
| `kernel/arch/x86_64/kbd.c` | Modify | Add `kbd_usb_inject(uint8_t ascii)` function |
| `kernel/arch/x86_64/kbd.h` | Modify | Declare `kbd_usb_inject()` |
| `kernel/arch/x86_64/pit.c` | Modify | Call `xhci_poll()` from PIT tick handler |
| `Makefile` | Modify | Add xhci.c, usb_hid.c to DRIVER_SRCS; update `make run` with USB flags |
| `tests/test_xhci.py` | Create | Boot with `-device qemu-xhci -device usb-kbd`, verify keyboard works |
| `tests/run_tests.sh` | Modify | Wire in test_xhci.py |

---

### Task 1: Create xhci.h — register structures and TRB definitions

**Files:**
- Create: `kernel/drivers/xhci.h`

- [ ] **Step 1: Define xHCI capability and operational register structures**

```c
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

/* USBCMD bits */
#define XHCI_CMD_RS                (1 << 0)
#define XHCI_CMD_HCRST             (1 << 1)
#define XHCI_CMD_INTE              (1 << 2)

/* USBSTS bits */
#define XHCI_STS_HCH               (1 << 0)
#define XHCI_STS_EINT              (1 << 3)

/* PORTSC bits */
#define XHCI_PORTSC_CCS            (1 << 0)
#define XHCI_PORTSC_PED            (1 << 1)
#define XHCI_PORTSC_PR             (1 << 4)
#define XHCI_PORTSC_PRC            (1 << 21)

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
```

- [ ] **Step 2: Commit**

```bash
git add kernel/drivers/xhci.h
git commit -m "drivers: add xHCI register structures and TRB definitions (xhci.h)"
```

---

### Task 2: Implement xhci.c — controller init and device enumeration

**Files:**
- Create: `kernel/drivers/xhci.c`

- [ ] **Step 1: Implement controller discovery and reset**

1. Call `pcie_find_device(0x0C, 0x03, 0x30)` — if not found, print skip and return
2. Map BAR0 via `kva_alloc_pages()` + `vmm_map_page()` loop
3. Read `CAPLENGTH` to locate operational registers at `BAR0 + CAPLENGTH`
4. Read `HCSPARAMS1` to get `MaxSlots` (bits [7:0]) and `MaxPorts` (bits [31:24])
5. Stop controller: clear `USBCMD.RS`, wait for `USBSTS.HCH=1`
6. Reset: set `USBCMD.HCRST`, wait for `USBCMD.HCRST` to clear
7. Read `DBOFF` and `RTSOFF` for doorbell and runtime register offsets

- [ ] **Step 2: Allocate DCBAA, Command Ring, Event Ring**

1. DCBAA: `(MaxSlots + 1) * 8` bytes, page-aligned. Allocate via `pmm_alloc_page()`, map to kernel VA, zero-fill. Write physical address to `DCBAAP`
2. Command Ring: `XHCI_CMD_RING_SIZE * 16` bytes. Allocate, zero, set up Link TRB at end pointing back to start. Write physical address to `CRCR`
3. Event Ring Segment: `XHCI_EVT_RING_SIZE * 16` bytes. Allocate, zero
4. Event Ring Segment Table (ERST): 1 entry = 16 bytes. Set `RingSegmentBaseAddress` and `RingSegmentSize = 64`
5. Write `ERSTSZ = 1`, then `ERSTBA`, then `ERDP` (event ring dequeue pointer)
6. Set `CONFIG.MaxSlotsEn = MaxSlots`
7. Set `USBCMD.RS = 1` to start controller

- [ ] **Step 3: Implement port scanning and device enumeration**

For each port (0 to MaxPorts-1):
1. Check `PORTSC.CCS` — skip if no device connected
2. Issue port reset: set `PORTSC.PR=1`, wait for `PORTSC.PRC=1` (reset complete)
3. Issue `Enable Slot` command on Command Ring → get slot ID from completion event
4. Allocate Input Context (device context + slot context + endpoint 0 context)
5. Issue `Address Device` command with Input Context → device gets USB address
6. Issue control transfer: `Get Descriptor` (Device Descriptor, 18 bytes) — check class/subclass
7. Issue `Get Descriptor` (Configuration Descriptor) — find interface with class=3 (HID), subclass=1 (boot), protocol=1 (keyboard)
8. Issue `Set Configuration` + `Set Protocol` (boot protocol) for HID keyboards
9. Configure interrupt IN endpoint via `Configure Endpoint` command
10. Schedule first interrupt IN transfer on the HID endpoint

- [ ] **Step 4: Implement xhci_poll**

Called from PIT handler at 100Hz:
1. Check event ring for new TRBs (compare phase bit)
2. For Transfer Event TRBs: extract slot ID and endpoint, pass received data to `usb_hid_process_report()`
3. Re-schedule interrupt IN transfer for continuous polling
4. Advance event ring dequeue pointer, write `ERDP`

- [ ] **Step 5: Build**

```bash
make 2>&1 | tail -20
```

- [ ] **Step 6: Commit**

```bash
git add kernel/drivers/xhci.c
git commit -m "drivers: implement xHCI controller init + device enumeration"
```

---

### Task 3: Create usb_hid.c — HID boot protocol keyboard

**Files:**
- Create: `kernel/drivers/usb_hid.h`
- Create: `kernel/drivers/usb_hid.c`

- [ ] **Step 1: Create usb_hid.h**

```c
/* usb_hid.h — USB HID boot-protocol keyboard driver
 *
 * Parses 8-byte HID boot reports and converts to ASCII.
 * Injects keystrokes into PS/2 keyboard ring buffer.
 */
#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

/* HID boot-protocol report: 8 bytes
 * [0] modifier keys (bitfield)
 * [1] reserved
 * [2..7] key codes (HID usage IDs, 0 = no key) */
typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keys[6];
} usb_hid_report_t;

/* Modifier key bits */
#define HID_MOD_LCTRL   (1 << 0)
#define HID_MOD_LSHIFT  (1 << 1)
#define HID_MOD_LALT    (1 << 2)
#define HID_MOD_LGUI    (1 << 3)
#define HID_MOD_RCTRL   (1 << 4)
#define HID_MOD_RSHIFT  (1 << 5)
#define HID_MOD_RALT    (1 << 6)
#define HID_MOD_RGUI    (1 << 7)

/* Process an incoming HID boot-protocol report.
 * Compares with previous report to detect key press/release events.
 * Calls kbd_usb_inject() for newly pressed keys. */
void usb_hid_process_report(const uint8_t *data, uint32_t len);

#endif /* USB_HID_H */
```

- [ ] **Step 2: Create usb_hid.c**

Implement the HID usage ID to ASCII lookup table. The USB HID usage table for keyboard maps usage IDs 0x04-0x27 to letters a-z, 0x1E-0x27 to digits 1-0, 0x28=Enter, 0x29=Escape, 0x2A=Backspace, 0x2C=Space, etc.

```c
/* usb_hid.c — HID boot-protocol keyboard driver */
#include "usb_hid.h"
#include "kbd.h"

/* Previous report for detecting key press/release transitions */
static usb_hid_report_t s_prev_report;

/* HID usage ID → ASCII (unshifted). Index = HID usage ID. */
static const char hid_to_ascii[128] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = '\n',  /* Enter */
    [0x29] = 0x1B,  /* Escape */
    [0x2A] = '\b',  /* Backspace */
    [0x2B] = '\t',  /* Tab */
    [0x2C] = ' ',   /* Space */
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']',
    [0x31] = '\\', [0x33] = ';', [0x34] = '\'', [0x35] = '`',
    [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

/* HID usage ID → ASCII (shifted) */
static const char hid_to_ascii_shift[128] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
    [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x29] = 0x1B, [0x2A] = '\b', [0x2B] = '\t',
    [0x2C] = ' ',
    [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}',
    [0x31] = '|', [0x33] = ':', [0x34] = '"', [0x35] = '~',
    [0x36] = '<', [0x37] = '>', [0x38] = '?',
};
```

Implement `usb_hid_process_report()`:
1. For each key in `report->keys[0..5]`, check if it was NOT in `s_prev_report.keys[]` (new key press)
2. For each newly pressed key, look up ASCII from `hid_to_ascii[]` (or `hid_to_ascii_shift[]` if shift modifier is set)
3. Handle Ctrl modifier: if Ctrl held and key is 'a'-'z', produce ASCII 1-26 (Ctrl-C = 3)
4. Call `kbd_usb_inject(ascii)` for each non-zero result
5. Save current report as `s_prev_report`

- [ ] **Step 3: Build**

```bash
make 2>&1 | tail -20
```

- [ ] **Step 4: Commit**

```bash
git add kernel/drivers/usb_hid.h kernel/drivers/usb_hid.c
git commit -m "drivers: add USB HID boot-protocol keyboard driver"
```

---

### Task 4: Wire xHCI poll into PIT handler and add kbd_usb_inject

**Files:**
- Modify: `kernel/arch/x86_64/kbd.c`
- Modify: `kernel/arch/x86_64/kbd.h`
- Modify: `kernel/arch/x86_64/pit.c`

- [ ] **Step 1: Add kbd_usb_inject() to kbd.c**

```c
/* Inject a USB HID keystroke into the PS/2 keyboard ring buffer.
 * Called from usb_hid_process_report() in interrupt context. */
void kbd_usb_inject(uint8_t ascii)
{
    if (ascii == 0) return;
    /* Push into the existing ring buffer used by PS/2 kbd_handler */
    uint16_t next = (s_ring_head + 1) % KBD_RING_SIZE;
    if (next == s_ring_tail) return;  /* buffer full */
    s_ring[s_ring_head] = ascii;
    s_ring_head = next;
    /* Wake any task blocked on kbd_read */
    if (s_waiter != NULL) {
        sched_unblock(s_waiter);
        s_waiter = NULL;
    }
}
```

- [ ] **Step 2: Declare kbd_usb_inject in kbd.h**

```c
void kbd_usb_inject(uint8_t ascii);
```

- [ ] **Step 3: Call xhci_poll() from PIT handler**

In `pit.c`, after the existing tick increment and scheduler call, add:

```c
    xhci_poll();    /* poll USB event ring for HID reports */
```

Guard with a global flag set by `xhci_init()` so the call is skipped when no xHCI controller is present.

- [ ] **Step 4: Wire xhci_init() into main.c**

After `nvme_init()`:

```c
#include "xhci.h"

    xhci_init();            /* xHCI USB host — [XHCI] OK or skip          */
```

- [ ] **Step 5: Update Makefile**

Add `xhci.c` and `usb_hid.c` to `DRIVER_SRCS`.

Update `make run` to add `-device qemu-xhci -device usb-kbd`.

- [ ] **Step 6: Build and run make test**

```bash
make test 2>&1 | tail -10
```

`make test` should remain GREEN — xHCI prints no line on `-machine pc`.

- [ ] **Step 7: Commit**

```bash
git add kernel/arch/x86_64/kbd.c kernel/arch/x86_64/kbd.h \
        kernel/arch/x86_64/pit.c kernel/core/main.c Makefile
git commit -m "phase22: wire xHCI poll into PIT handler, add kbd_usb_inject"
```

---

### Task 5: Write test_xhci.py and wire into run_tests.sh

**Files:**
- Create: `tests/test_xhci.py`
- Modify: `tests/run_tests.sh`

- [ ] **Step 1: Create test_xhci.py**

Boot with `-machine q35 -device qemu-xhci -device usb-kbd` and send keyboard input via QEMU monitor `sendkey` commands. Verify the shell responds — this proves the USB HID → kbd ring buffer → VFS → shell pipeline works.

Test flow:
1. Boot QEMU with q35 + xHCI + USB keyboard
2. Wait for shell prompt
3. Send `echo hello` via `sendkey` commands
4. Verify "hello" appears in serial output

Follow `test_pipe.py` monitor socket pattern.

- [ ] **Step 2: Wire into run_tests.sh**

```bash
echo "--- test_xhci ---"
python3 tests/test_xhci.py || FAIL=1
```

- [ ] **Step 3: Run the test**

```bash
python3 tests/test_xhci.py
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_xhci.py tests/run_tests.sh
git commit -m "tests: add xHCI USB keyboard test (test_xhci.py)"
```

---

### Task 6: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 22 to Build Status table**

```markdown
| xHCI + USB HID keyboard (Phase 22) | ✅ Done | xHCI init on q35; USB HID boot protocol; kbd_usb_inject into PS/2 ring buffer; test_xhci.py PASS |
```

- [ ] **Step 2: Add Phase 22 forward-looking constraints**

```markdown
### Phase 22 forward-looking constraints

**Polling-only at 100Hz.** USB HID reports are polled from the PIT tick handler. This adds ~1-2us per tick when no events are pending. MSI-X driven completion is v2.0 work.

**Single keyboard only.** The first HID boot-protocol keyboard found is used. Additional keyboards on other ports are ignored.

**No USB hub support.** Devices must be connected directly to root hub ports. Hub enumeration (Set Hub Feature, Get Port Status) is v2.0 work.

**No USB mass storage.** Only HID class devices are handled. USB storage class driver is v2.0 work.

**PS/2 and USB coexist.** Both paths push into the same ring buffer. On real hardware without PS/2, only USB HID provides input. On QEMU, QEMU's `sendkey` routes to whichever keyboard is active.

**Transfer ring memory is never freed.** Each device slot's transfer ring is allocated permanently. USB device disconnect does not reclaim resources.
```

- [ ] **Step 3: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md build status for Phase 22"
```

---

## Final Verification

```bash
make test 2>&1 | tail -5
```

Expected: exit 0 (boot.txt unchanged — xHCI adds no line on `-machine pc`).

```bash
python3 tests/test_xhci.py
```

Expected: PASS — keyboard input via USB HID produces shell output.
