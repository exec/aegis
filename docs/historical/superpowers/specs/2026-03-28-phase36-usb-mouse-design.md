# Phase 36: USB HID Mouse + Installer Password Hashing

## Goal

Add USB HID boot protocol mouse support via the existing xHCI driver, expose mouse events to userspace through `/dev/mouse`, support hotplug detection, and fix the installer to hash custom passwords properly.

## Scope

1. **xHCI device type detection** — distinguish keyboard (protocol 1) from mouse (protocol 2) during USB enumeration
2. **Mouse report parser** — new `kernel/drivers/usb_mouse.c` parses 3-byte boot protocol reports into delta events
3. **`/dev/mouse` VFS device** — ring buffer of `mouse_event_t` structs, blocking + non-blocking read
4. **USB hotplug** — Port Status Change events in `xhci_poll()` trigger silent enumeration
5. **Installer password hashing** — call `crypt()` to hash the entered password instead of using a hardcoded hash
6. **Installer password masking** — echo `*` per character during password input (same termios technique as login)
7. **Test binary + QEMU test** — `user/mouse_test/main.c` + `test_mouse.py`

## Non-Goals

- Cursor rendering (Lumen Phase 37)
- Cursor position tracking in kernel (Lumen's job — kernel delivers raw deltas)
- Scroll wheel for 3-byte boot protocol mice (4th byte only in extended reports)
- PS/2 mouse support (future work)
- Post-boot printk suppression (Phase 37 Lumen concern)

---

## 1. Device Type Detection

### Current State

`enumerate_port()` in `xhci.c` addresses every HID device and configures EP1 IN for 8-byte interrupt transfers. `xhci_poll()` dispatches all transfer events to `usb_hid_process_report()` (keyboard parser).

### Changes

During `enumerate_port()`, after Address Device succeeds, issue a GET_DESCRIPTOR(Configuration) control transfer. Parse the returned configuration descriptor to find the HID interface descriptor. Check `bInterfaceProtocol`:

| Value | Meaning | Handler |
|-------|---------|---------|
| 1 | Keyboard | `usb_hid_process_report()` (existing) |
| 2 | Mouse | `usb_mouse_process_report()` (new) |
| other | Unknown | Skip (don't configure) |

Store device type per slot:

```c
#define USB_DEV_NONE  0
#define USB_DEV_KBD   1
#define USB_DEV_MOUSE 2

static uint8_t s_hid_slot_type[XHCI_MAX_SLOTS];
```

In `xhci_poll()`, dispatch based on `s_hid_slot_type[slot]`:

```c
if (s_hid_slot_type[slot] == USB_DEV_KBD)
    usb_hid_process_report(buf, len);
else if (s_hid_slot_type[slot] == USB_DEV_MOUSE)
    usb_mouse_process_report(buf, len);
```

Mouse boot protocol reports are 3 bytes. Configure EP1 IN with `max_packet_size=8` (same as keyboard — USB spec mandates 8-byte minimum for low-speed interrupt endpoints; the controller pads short reports).

### SET_PROTOCOL(Boot)

After detecting a mouse, issue SET_PROTOCOL(0) to force boot protocol mode. This ensures the device sends the standard 3-byte format regardless of any HID report descriptor complexity. Same as keyboards — boot protocol is simple and universal.

---

## 2. Mouse Report Parser

### New File: `kernel/drivers/usb_mouse.c`

**Boot protocol mouse report (3 bytes):**

| Byte | Content |
|------|---------|
| 0 | Buttons: bit 0 = left, bit 1 = right, bit 2 = middle |
| 1 | X delta (int8_t): positive = right |
| 2 | Y delta (int8_t): positive = down |

**Event struct (delivered to userspace):**

```c
typedef struct {
    uint8_t  buttons;   /* button state bitmap */
    int16_t  dx;        /* X delta (pixels) */
    int16_t  dy;        /* Y delta (pixels) */
    int16_t  scroll;    /* reserved, always 0 for boot protocol */
} __attribute__((packed)) mouse_event_t;  /* 7 bytes */
```

**Ring buffer:** 128 `mouse_event_t` entries. Static allocation in BSS (~900 bytes).

**API:**

```c
void usb_mouse_process_report(const uint8_t *data, uint32_t len);
int  mouse_poll(mouse_event_t *out);    /* non-blocking, returns 0 if empty */
void mouse_read_blocking(mouse_event_t *out);  /* blocks until event */
```

`usb_mouse_process_report()`:
1. Validate `len >= 3`
2. Extract buttons, dx (int8_t), dy (int8_t)
3. Push `mouse_event_t` into ring buffer
4. Wake any sleeping reader (same pattern as `kbd_usb_inject` → `buf_push`)

No printk during report processing. Ever.

---

## 3. `/dev/mouse` VFS Device

### Registration

New file: `kernel/fs/mouse_vfs.c` (or inline in usb_mouse.c — follow kbd_vfs pattern).

`mouse_vfs_open()` returns a `vfs_file_t` with:
- `read`: returns `mouse_event_t` structs (7 bytes each). Partial reads return whole events only. If buffer has 3 events and caller requests 21 bytes, return all 3 (21 bytes). If caller requests 10 bytes, return 1 event (7 bytes).
- `write`: returns `-ENOSYS`
- `close`: no-op (stateless, single shared buffer)
- `stat`: `S_IFCHR`, mode `0444`

### VFS Path

Opened via `open("/dev/mouse", O_RDONLY)`. Registered in `vfs_open()` device table alongside `/dev/tty`, `/dev/ptmx`, `/dev/urandom`.

### Blocking Behavior

- Default (no `O_NONBLOCK`): `read()` blocks the calling task until at least one event is available. Uses `sched_block()` / `sched_unblock()` pattern.
- `O_NONBLOCK`: returns `-EAGAIN` if ring buffer is empty.

### Capability Gate

`CAP_KIND_VFS_READ` — same as keyboard. No new capability type needed.

---

## 4. USB Hotplug

### Port Status Change Events

xHCI controllers generate Port Status Change Event TRBs (type 34) when a device is connected or disconnected. Currently `xhci_poll()` only handles Transfer Event TRBs (type 32).

Add a handler for type 34:

```c
case XHCI_TRB_PORT_STATUS_CHANGE: {
    uint8_t port_id = /* extract from TRB */;
    uint32_t portsc = read_portsc(port_id);
    if (portsc & PORTSC_CSC) {  /* Connect Status Change */
        clear_portsc_csc(port_id);
        if (portsc & PORTSC_CCS) {
            /* Device connected — enumerate silently */
            enumerate_port(port_id);  /* no printk */
        } else {
            /* Device disconnected — deactivate slot */
            deactivate_slot_for_port(port_id);
        }
    }
    break;
}
```

### Silent Enumeration

`enumerate_port()` currently calls `printk()` for status messages. Add a `silent` parameter or check a global `s_post_boot` flag. After `xhci_init()` completes, set `s_post_boot = 1`. All subsequent enumerations skip printk entirely (both serial and VGA). Debug-level hotplug logging can be added behind `#ifdef USB_DEBUG` if needed during development.

### Disconnect Handling

When a device disconnects:
1. Mark `s_hid_slot_type[slot] = USB_DEV_NONE`
2. Stop scheduling interrupt IN transfers for that slot
3. No printk

---

## 5. Installer Password Hashing

### Current Problem

`user/installer/main.c` prompts for a password but ignores it, writing a hardcoded SHA-512 hash for "forevervigilant" to `/etc/shadow`.

### Fix

Call `crypt(password, salt)` to hash the entered password. musl provides `crypt()` which supports SHA-512 (`$6$` prefix).

**Salt generation:** Use `/dev/urandom` to generate 16 random bytes, base64-encode into a salt string, prepend `$6$`.

```c
/* Generate salt from /dev/urandom */
uint8_t rand_bytes[12];
int ufd = open("/dev/urandom", O_RDONLY);
read(ufd, rand_bytes, 12);
close(ufd);

/* Base64-encode into salt (16 chars) */
char salt[32];
snprintf(salt, sizeof(salt), "$6$%.16s$", base64(rand_bytes));

/* Hash */
char *hashed = crypt(entered_password, salt);

/* Write to /etc/shadow */
snprintf(line, sizeof(line), "%s:%s:19814:0:99999:7:::\n", username, hashed);
```

### Link Requirement

musl's `crypt()` requires linking with `-lcrypt`. Update `user/installer/Makefile` to add `-lcrypt`.

---

## 6. Installer Password Masking

### Current Problem

The installer uses plain `fgets()` for password input — characters are echoed in cleartext.

### Fix

Use the same termios raw mode technique as `user/login/main.c`:

1. `tcgetattr(0, &orig)` — save terminal state
2. Clear `ECHO | ICANON` flags
3. `tcsetattr(0, TCSANOW, &raw)` — apply
4. Read one character at a time with `read(0, &c, 1)`
5. For each printable character: store in buffer, write `*` to stdout
6. Handle backspace: erase last `*`, decrement buffer position
7. On `\n`/`\r`: write `\n`, null-terminate, restore terminal

Same code can be extracted into a `read_password()` helper used by both prompts (password + confirmation).

---

## 7. Testing

### test_mouse.py

**QEMU setup:** `-machine q35 -device qemu-xhci -device usb-mouse`

**Test binary:** `user/mouse_test/main.c`
- Opens `/dev/mouse` with `O_RDONLY`
- Reads events in a loop, prints `btn=XX dx=NN dy=NN` per event
- Exits after 5 events or 3-second timeout

**Test script flow:**
1. Boot QEMU with USB mouse device
2. Wait for shell prompt
3. Run `mouse_test &`
4. Inject mouse movement via QEMU monitor: `mouse_move 10 20`
5. Inject mouse click: `mouse_button 1` then `mouse_button 0`
6. Capture stdout, verify event output contains expected deltas
7. PASS if at least 2 events with non-zero dx/dy received

### Installer Password Test

Extend `test_installer.py` to verify:
- Entered password works on next boot (login succeeds with custom password)
- Pre-computed "forevervigilant" no longer works (unless that's what was entered)

### Boot Oracle

No changes to `tests/expected/boot.txt`. Mouse init happens silently. The `[XHCI]` line is unchanged.

---

## File Changes Summary

| File | Change |
|------|--------|
| `kernel/drivers/xhci.c` | Device type detection, hotplug PSC handler, silent enum flag |
| `kernel/drivers/xhci.h` | `USB_DEV_NONE/KBD/MOUSE` constants, `usb_mouse_process_report` decl |
| `kernel/drivers/usb_mouse.c` | **New.** Report parser, ring buffer, blocking read |
| `kernel/drivers/usb_mouse.h` | **New.** `mouse_event_t`, API declarations |
| `kernel/fs/vfs.c` | Register `/dev/mouse` open path |
| `kernel/core/main.c` | No change (mouse init is part of xhci_init via enumeration) |
| `user/mouse_test/main.c` | **New.** Test binary — open /dev/mouse, read events, print |
| `user/installer/main.c` | `crypt()` password hashing, termios asterisk masking |
| `user/installer/Makefile` | Add `-lcrypt` |
| `tests/test_mouse.py` | **New.** QEMU USB mouse test |
| `Makefile` | Add mouse_test build rule, wire test_mouse.py |

---

## Forward Constraints

1. **No cursor rendering.** Kernel delivers raw deltas. Cursor position tracking and rendering are Lumen's job (Phase 37).
2. **Single mouse only.** First boot-protocol mouse found is used. Additional mice on other ports are ignored.
3. **No scroll wheel in boot protocol.** The `scroll` field in `mouse_event_t` is always 0. Extended HID reports (with scroll) require HID report descriptor parsing — future work.
4. **No PS/2 mouse.** Only USB HID via xHCI.
5. **Mouse event ring buffer is never freed.** Static BSS allocation, permanent.
6. **Hotplug has no debounce.** Rapid connect/disconnect could flood the event ring. Not a concern for single-device usage.
7. **`crypt()` is slow.** SHA-512 with salt takes ~50ms on real hardware. Acceptable for installer — not for a high-frequency auth path.
