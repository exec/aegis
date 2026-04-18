# GUI Test Harness — Dock Click Automation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** End-to-end GUI test that boots Aegis, parses dock icon coordinates from serial, drives the QEMU PS/2 mouse to click each icon, and verifies the right app window opened.

**Architecture:** Three cooperating pieces — (a) Vortex gains `mouse_move`/`mouse_button` HMP wrappers next to the existing `screendump`/`send_keys`; (b) Lumen emits one `[DOCK] item=<key> cx=.. cy=..` line per dock icon on ready plus a `[LUMEN] window_opened=<key>` line on successful click dispatch; (c) a new `dock_click_test.rs` scrapes the dock lines, sends HMP mouse commands to each center, waits for the window_opened ack, and screendumps the post-click frame.

**Tech Stack:** Rust (tokio, vortex path dep), C (user/bin/lumen, user/lib/citadel), QEMU HMP over Unix socket, PS/2 mouse via i8042.

**Spec:** `docs/superpowers/specs/2026-04-09-gui-test-dock-click-design.md`

**Cross-repo note:** Task 1 modifies `/Users/dylan/Developer/vortex/` (sibling of aegis). All other tasks modify `/Users/dylan/Developer/aegis/`. The aegis tests crate picks up vortex changes via the `path = "../../vortex"` dependency, so no version bump is needed.

**Implementation note on HMP mouse semantics:** QEMU's HMP `mouse_move dx dy` sends *relative* deltas regardless of whether the guest uses PS/2 or USB HID. There is no absolute-positioning HMP command for the relative PS/2 mouse Aegis supports. The test must therefore "home" the cursor to (0, 0) by sending a large negative delta (e.g. `-5000 -5000` twice) before moving to an absolute target. Lumen clamps cursor_x/cursor_y to the screen bounds on every packet, so homing converges to (0, 0) regardless of the current position. Each click is a 4-step sequence: home, move-to-target, press, release.

---

## File Structure

**Create:**
- `/Users/dylan/Developer/aegis/tests/tests/dock_click_test.rs` — end-to-end test
- `/Users/dylan/Developer/aegis/tests/tests/mouse_api_smoke_test.rs` — smoke test for Task 1

**Modify:**
- `/Users/dylan/Developer/vortex/src/core/qemu.rs` — add `mouse_move` + `mouse_button`
- `/Users/dylan/Developer/aegis/user/lib/citadel/dock.c` — add key table + emit function
- `/Users/dylan/Developer/aegis/user/lib/citadel/dock.h` — declare new functions
- `/Users/dylan/Developer/aegis/user/bin/lumen/main.c` — call emit + print `window_opened`
- `/Users/dylan/Developer/aegis/tests/src/presets.rs` — add graphical-with-mouse preset
- `/Users/dylan/Developer/aegis/tests/src/lib.rs` — re-export new preset

---

## Task 1: Vortex mouse HMP primitives

**Files:**
- Modify: `/Users/dylan/Developer/vortex/src/core/qemu.rs` — add methods on `QemuProcess`
- Create: `/Users/dylan/Developer/aegis/tests/tests/mouse_api_smoke_test.rs` — smoke test

**Context:** `QemuProcess::screendump` (qemu.rs:190-261) and `QemuProcess::send_keys` (qemu.rs:271-329) already implement the "open Unix socket, drain banner, write HMP command, wait for `(qemu)` prompt" pattern. The new methods follow the exact same pattern.

- [ ] **Step 1: Write the failing smoke test**

Create `/Users/dylan/Developer/aegis/tests/tests/mouse_api_smoke_test.rs`:

```rust
// Smoke test: verify vortex's new mouse_move / mouse_button methods
// round-trip successfully against a real QEMU instance. This test
// doesn't verify that the mouse event reached the guest — just that
// the HMP command was accepted without error. The dock_click_test
// covers end-to-end delivery.

use aegis_tests::{iso, AegisHarness};
use std::time::Duration;
use vortex::core::config::QemuOpts;

fn opts() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:16".into(),
        devices: vec![
            "virtio-vga".into(),
            "qemu-xhci,id=xhci".into(),
            "usb-kbd,bus=xhci.0".into(),
        ],
        drives: vec![],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}

#[tokio::test]
async fn mouse_move_and_button_round_trip() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (_stream, mut proc) = AegisHarness::boot_stream(opts(), &iso)
        .await
        .expect("QEMU failed to start");

    // Give QEMU a moment to fully initialize before hitting the monitor.
    tokio::time::sleep(Duration::from_millis(500)).await;

    proc.mouse_move(10, 10).await.expect("mouse_move failed");
    proc.mouse_button(0).await.expect("mouse_button(0) failed");

    proc.kill().await.unwrap();
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /Users/dylan/Developer/aegis && cargo test --manifest-path tests/Cargo.toml --test mouse_api_smoke_test -- --nocapture
```

Expected: compilation error `no method named 'mouse_move' found for struct 'QemuProcess'`.

- [ ] **Step 3: Add the two methods to Vortex**

Edit `/Users/dylan/Developer/vortex/src/core/qemu.rs`. Add these three methods inside `impl QemuProcess { ... }`, immediately after `send_keys` (around line 329, before the closing `}` of the impl block and before `cleanup_monitor_socket`):

```rust
    /// Send an HMP `mouse_move` command with the given relative deltas.
    ///
    /// QEMU's HMP `mouse_move` operates on the currently active pointing
    /// device and always sends *relative* deltas — there is no absolute
    /// form. Callers that need to reach a specific pixel must home the
    /// cursor first by sending a large negative delta, relying on the
    /// guest clamping to (0, 0), before sending the target delta.
    ///
    /// Requires `monitor_socket = true` in QemuOpts.
    pub async fn mouse_move(&self, dx: i32, dy: i32) -> Result<()> {
        self.send_monitor_cmd(&format!("mouse_move {} {}", dx, dy)).await
    }

    /// Send an HMP `mouse_button` command with the given button mask.
    ///
    /// The mask is a bitmap of held buttons: 1 = left, 2 = right, 4 = middle.
    /// To click, callers send `mouse_button(1)` followed by `mouse_button(0)`.
    ///
    /// Requires `monitor_socket = true` in QemuOpts.
    pub async fn mouse_button(&self, mask: u32) -> Result<()> {
        self.send_monitor_cmd(&format!("mouse_button {}", mask)).await
    }

    /// Private helper: open the monitor socket, drain the banner, write
    /// one HMP command, and wait for the `(qemu)` prompt indicating the
    /// command has been processed. Returns on success or timeout.
    async fn send_monitor_cmd(&self, command: &str) -> Result<()> {
        let socket_path = self.monitor_socket_path.as_ref().ok_or_else(|| {
            VortexError::QemuError {
                message: format!(
                    "{} requires monitor_socket = true in QemuOpts",
                    command.split_whitespace().next().unwrap_or(command)
                ),
            }
        })?;

        let mut stream = tokio::net::UnixStream::connect(socket_path)
            .await
            .map_err(|e| VortexError::QemuError {
                message: format!("failed to connect to QEMU monitor socket: {}", e),
            })?;

        // Drain initial banner/prompt from QEMU monitor.
        let mut buf = vec![0u8; 4096];
        let _ = tokio::time::timeout(
            std::time::Duration::from_millis(500),
            stream.read(&mut buf),
        )
        .await;

        let cmd = format!("{}\n", command);
        stream.write_all(cmd.as_bytes()).await.map_err(|e| {
            VortexError::QemuError {
                message: format!("failed to send {} command: {}", command, e),
            }
        })?;

        // Wait for (qemu) prompt indicating command completed.
        let mut response = Vec::new();
        let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(5);

        loop {
            let remaining = deadline.saturating_duration_since(tokio::time::Instant::now());
            if remaining.is_zero() {
                return Err(VortexError::QemuError {
                    message: format!("timeout waiting for QEMU monitor response to {}", command),
                });
            }

            match tokio::time::timeout(remaining, stream.read(&mut buf)).await {
                Ok(Ok(0)) => break,
                Ok(Ok(n)) => {
                    response.extend_from_slice(&buf[..n]);
                    if response.windows(6).any(|w| w == b"(qemu)") {
                        break;
                    }
                }
                Ok(Err(e)) => {
                    return Err(VortexError::QemuError {
                        message: format!("error reading QEMU monitor: {}", e),
                    });
                }
                Err(_) => {
                    return Err(VortexError::QemuError {
                        message: format!("timeout waiting for QEMU monitor response to {}", command),
                    });
                }
            }
        }

        Ok(())
    }
```

- [ ] **Step 4: Run the smoke test to verify it passes**

```bash
cd /Users/dylan/Developer/aegis && cargo test --manifest-path tests/Cargo.toml --test mouse_api_smoke_test -- --nocapture
```

Expected: `test mouse_move_and_button_round_trip ... ok` with the test printing nothing on stderr other than QEMU's own noise.

If the test fails with "timeout waiting for QEMU monitor response": the HMP command name is wrong. Run `qemu-system-x86_64 -monitor stdio -machine q35 -display none` locally and type `help mouse_move` to verify the exact command syntax on your QEMU build.

- [ ] **Step 5: Commit Vortex change and smoke test separately**

Vortex repo first:

```bash
cd /Users/dylan/Developer/vortex && git add src/core/qemu.rs && git commit -m "$(cat <<'EOF'
feat(qemu): add mouse_move and mouse_button HMP wrappers

Thin wrappers mirroring the screendump/send_keys pattern, backed by a
new private send_monitor_cmd helper that drains the banner, writes an
HMP command, and waits for the (qemu) prompt.

mouse_move is relative-only (HMP has no absolute form for relative
pointing devices). Callers that need to reach a specific pixel must
home the cursor by sending a large negative delta first.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

Then aegis repo:

```bash
cd /Users/dylan/Developer/aegis && git add tests/tests/mouse_api_smoke_test.rs && git commit -m "$(cat <<'EOF'
test: add smoke test for vortex mouse_move/mouse_button

Verifies the new vortex HMP wrappers round-trip successfully against a
real QEMU instance. End-to-end delivery is covered by dock_click_test.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Dock item key table and debug emit

**Files:**
- Modify: `/Users/dylan/Developer/aegis/user/lib/citadel/dock.c`
- Modify: `/Users/dylan/Developer/aegis/user/lib/citadel/dock.h`

**Context:** `dock.h:25-29` defines four item constants (`DOCK_ITEM_SETTINGS=0`, `DOCK_ITEM_FILES=1`, `DOCK_ITEM_TERMINAL=2`, `DOCK_ITEM_WIDGETS=3`). The `dock_item_rect` helper at `dock.c:28-33` computes the icon's top-left (ix, iy); size is always `DOCK_ICON_SIZE` (48×48). Screen-space center is `(ix + 24, iy + 24)` and half-extent is 24 on each axis. No failing test — this is infrastructure consumed by Task 3.

- [ ] **Step 1: Add the key table and two functions to dock.c**

Edit `/Users/dylan/Developer/aegis/user/lib/citadel/dock.c`. First, add `#include <stdio.h>` near the top. The file currently has:

```c
#include "dock.h"
#include "lumen_theme.h"
#include <glyph.h>
#include <font.h>
#include <stdlib.h>
#include <string.h>
```

Add one line after `#include <string.h>`:

```c
#include <stdio.h>
```

Then, immediately after the existing static variable declarations (after `static int s_hover_item = -1;` at line 11, before the `dock_init` function), add:

```c
/* Keys used by the GUI test harness to identify dock items by string
 * rather than by raw integer index. Order must match the DOCK_ITEM_*
 * defines in dock.h. */
static const char *s_dock_item_keys[DOCK_ITEM_COUNT] = {
    [DOCK_ITEM_SETTINGS] = "settings",
    [DOCK_ITEM_FILES]    = "files",
    [DOCK_ITEM_TERMINAL] = "terminal",
    [DOCK_ITEM_WIDGETS]  = "widgets",
};
```

Then, at the end of dock.c (after `dock_get_rect` which ends around line 256), add:

```c
const char *
dock_item_key(int item)
{
    if (item < 0 || item >= DOCK_ITEM_COUNT)
        return "unknown";
    return s_dock_item_keys[item];
}

void
dock_emit_debug_lines(void)
{
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        int ix, iy;
        dock_item_rect(i, &ix, &iy);
        int cx = ix + DOCK_ICON_SIZE / 2;
        int cy = iy + DOCK_ICON_SIZE / 2;
        int hw = DOCK_ICON_SIZE / 2;
        int hh = DOCK_ICON_SIZE / 2;
        dprintf(2, "[DOCK] item=%s idx=%d cx=%d cy=%d hw=%d hh=%d\n",
                s_dock_item_keys[i], i, cx, cy, hw, hh);
    }
    dprintf(2, "[DOCK] ready\n");
}
```

Note: `dock_item_rect` is currently `static`. That's fine — `dock_emit_debug_lines` lives in the same translation unit.

- [ ] **Step 2: Declare the two new functions in dock.h**

Edit `/Users/dylan/Developer/aegis/user/lib/citadel/dock.h`. Add two declarations after the existing `dock_get_rect` declaration (currently around line 35, before `#endif`):

```c
const char *dock_item_key(int item);       /* key string for DOCK_ITEM_* */
void dock_emit_debug_lines(void);          /* print [DOCK] lines to stderr */
```

- [ ] **Step 3: Verify it compiles**

The citadel library is rebuilt as part of a normal `make` invocation. On the x86 build box (see Task 6), run:

```bash
make 2>&1 | tail -30
```

Expected: clean build. If `dprintf` is missing a prototype include, the earlier `#include <stdio.h>` addition covers it.

- [ ] **Step 4: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add user/lib/citadel/dock.c user/lib/citadel/dock.h && git commit -m "$(cat <<'EOF'
feat(citadel): add dock_item_key and dock_emit_debug_lines

Expose a stable string key per dock item and a debug emission helper
that dumps each icon's center and half-extent to stderr. Used by the
GUI test harness to drive clicks at known coordinates without having
to compile-time probe dock geometry.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Lumen debug output integration

**Files:**
- Modify: `/Users/dylan/Developer/aegis/user/bin/lumen/main.c`

**Context:** Lumen prints `[LUMEN] ready` at main.c:434 after the fade-in completes. The dock click dispatch lives at main.c:715-737. Files is a stub (just falls through to the mouse handler). Terminal, Widgets, and Settings each allocate a window and add it to the compositor.

- [ ] **Step 1: Call dock_emit_debug_lines right after [LUMEN] ready**

Edit `/Users/dylan/Developer/aegis/user/bin/lumen/main.c`. Find the existing line:

```c
    /* Signal test harness: fade-in complete, desktop is on screen */
    dprintf(2, "[LUMEN] ready\n");
```

(currently at line 433-434). Change it to:

```c
    /* Signal test harness: fade-in complete, desktop is on screen */
    dprintf(2, "[LUMEN] ready\n");
    dock_emit_debug_lines();
```

- [ ] **Step 2: Print window_opened on successful dock click dispatch**

Still in `/Users/dylan/Developer/aegis/user/bin/lumen/main.c`, find the dock click block (around line 715-737). The current code is:

```c
                    /* Dock click */
                    if (dock_item >= 0) {
                        if (dock_item == DOCK_ITEM_TERMINAL)
                            spawn_terminal(&comp, fb_w, fb_h);
                        else if (dock_item == DOCK_ITEM_WIDGETS) {
                            glyph_window_t *wt = widget_test_create();
                            if (wt) {
                                wt->x = (fb_w - wt->surf_w) / 2;
                                wt->y = (fb_h - wt->surf_h) / 2;
                                comp_add_window(&comp, wt);
                                comp_raise_window(&comp, wt);
                                comp.focused = wt;
                                wt->focused_window = 1;
                                glyph_window_mark_all_dirty(wt);
                            }
                        }
                        else if (dock_item == DOCK_ITEM_SETTINGS)
                            open_settings(&comp, fb_w, fb_h);
                        /* Files is a stub */
                        comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                        activity = 1;
                        goto after_mouse;
                    }
```

Change the body so each non-stub branch emits a `[LUMEN] window_opened=<key>` line *after* its create/spawn call:

```c
                    /* Dock click */
                    if (dock_item >= 0) {
                        if (dock_item == DOCK_ITEM_TERMINAL) {
                            spawn_terminal(&comp, fb_w, fb_h);
                            dprintf(2, "[LUMEN] window_opened=%s\n",
                                    dock_item_key(dock_item));
                        }
                        else if (dock_item == DOCK_ITEM_WIDGETS) {
                            glyph_window_t *wt = widget_test_create();
                            if (wt) {
                                wt->x = (fb_w - wt->surf_w) / 2;
                                wt->y = (fb_h - wt->surf_h) / 2;
                                comp_add_window(&comp, wt);
                                comp_raise_window(&comp, wt);
                                comp.focused = wt;
                                wt->focused_window = 1;
                                glyph_window_mark_all_dirty(wt);
                                dprintf(2, "[LUMEN] window_opened=%s\n",
                                        dock_item_key(dock_item));
                            }
                        }
                        else if (dock_item == DOCK_ITEM_SETTINGS) {
                            open_settings(&comp, fb_w, fb_h);
                            dprintf(2, "[LUMEN] window_opened=%s\n",
                                    dock_item_key(dock_item));
                        }
                        /* Files is a stub — no window_opened line */
                        comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                        activity = 1;
                        goto after_mouse;
                    }
```

Key changes: each of the three non-stub branches is now wrapped in `{ ... }` (so the `dprintf` is in scope) and prints `[LUMEN] window_opened=<key>`. The Widgets branch prints only if `wt` is non-null (successful allocation). Files deliberately has no print; the test handles this by not expecting one.

- [ ] **Step 3: Build and visual-check in interactive mode (optional — skip if building on remote box)**

Run `make run` locally (if supported) or on the x86 box and click a dock icon manually. Confirm the serial log shows:

```
[LUMEN] ready
[DOCK] item=settings idx=0 cx=... cy=... hw=24 hh=24
[DOCK] item=files idx=1 cx=... cy=... hw=24 hh=24
[DOCK] item=terminal idx=2 cx=... cy=... hw=24 hh=24
[DOCK] item=widgets idx=3 cx=... cy=... hw=24 hh=24
[DOCK] ready
...
[LUMEN] window_opened=terminal
```

If the `[DOCK]` lines are missing, `dock_emit_debug_lines` isn't being linked — check that `user/lib/citadel/Makefile` includes `dock.c` (it already does; this is a sanity check).

- [ ] **Step 4: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add user/bin/lumen/main.c && git commit -m "$(cat <<'EOF'
feat(lumen): emit dock geometry and window_opened for test harness

On [LUMEN] ready, dump each dock icon's center coordinates via
dock_emit_debug_lines(). On successful click dispatch for Terminal,
Widgets, or Settings, print [LUMEN] window_opened=<key>. Files is a
stub and deliberately prints nothing.

This lets the new dock_click_test scrape coordinates from serial
output, drive QEMU mouse HMP commands to each icon, and gate the
post-click screenshot on a clear success signal.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Graphical q35 preset with mouse enabled

**Files:**
- Modify: `/Users/dylan/Developer/aegis/tests/src/presets.rs`
- Modify: `/Users/dylan/Developer/aegis/tests/src/lib.rs`

**Context:** `aegis_q35()` at presets.rs:32-58 adds `usb-mouse,bus=xhci.0` and uses `display: "none"`. For GUI automation we need the virtio-vga framebuffer and VNC display (to render pixels), the monitor socket (to send HMP), the PS/2 mouse (not USB — HMP `mouse_move` targets the PS/2 i8042 AUX device which q35 provides by default), and no NVMe drive (we don't need disk for this test). This matches screendump_test.rs's `graphical_opts()` plus `monitor_socket = true` (already set there) plus the explicit absence of `usb-mouse`.

- [ ] **Step 1: Add the preset function to presets.rs**

Edit `/Users/dylan/Developer/aegis/tests/src/presets.rs`. Append this function after the existing `aegis_q35` function (after its closing `}` at around line 58, before the `#[cfg(test)]` block):

```rust
/// Graphical q35 preset with framebuffer + monitor socket + PS/2 mouse.
///
/// Differs from `aegis_q35` in: uses virtio-vga instead of std VGA,
/// enables `monitor_socket` so tests can drive HMP `screendump` /
/// `mouse_move` / `mouse_button`, omits NVMe/virtio-net, does not add
/// `usb-mouse` (q35 exposes a PS/2 i8042 AUX channel by default, and
/// HMP mouse commands target that device). Intended for GUI tests
/// that need to click and screenshot.
pub fn aegis_q35_graphical_mouse() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:17".into(),
        devices: vec![
            "virtio-vga".into(),
            "qemu-xhci,id=xhci".into(),
            "usb-kbd,bus=xhci.0".into(),
        ],
        drives: vec![],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}
```

- [ ] **Step 2: Re-export from lib.rs**

Edit `/Users/dylan/Developer/aegis/tests/src/lib.rs`. The current line is:

```rust
pub use presets::{aegis_pc, aegis_q35, disk, iso};
```

Change it to:

```rust
pub use presets::{aegis_pc, aegis_q35, aegis_q35_graphical_mouse, disk, iso};
```

- [ ] **Step 3: Verify the crate still compiles**

```bash
cd /Users/dylan/Developer/aegis && cargo check --manifest-path tests/Cargo.toml
```

Expected: clean check, no warnings.

- [ ] **Step 4: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add tests/src/presets.rs tests/src/lib.rs && git commit -m "$(cat <<'EOF'
test: add aegis_q35_graphical_mouse preset

Graphical q35 preset with virtio-vga, monitor socket, and PS/2 mouse
enabled (no usb-mouse). Used by the upcoming dock_click_test to drive
HMP mouse_move/mouse_button against the emulated PS/2 AUX channel
while capturing frames over the monitor socket.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: dock_click_test — end-to-end

**Files:**
- Create: `/Users/dylan/Developer/aegis/tests/tests/dock_click_test.rs`

**Context:** The test must boot Aegis, wait for `[LUMEN] ready`, collect `[DOCK] item=...` lines until `[DOCK] ready`, parse them into a map, then for each non-stub key (terminal, widgets, settings) home the cursor, move to target, click, wait for `[LUMEN] window_opened=<key>`, and screendump. All parsing is manual — no regex crate.

- [ ] **Step 1: Write the test file**

Create `/Users/dylan/Developer/aegis/tests/tests/dock_click_test.rs`:

```rust
// End-to-end GUI test: boots Aegis graphically, scrapes dock icon
// coordinates from the serial log, drives the QEMU PS/2 mouse via HMP
// to click each icon, waits for the corresponding
// [LUMEN] window_opened=<key> acknowledgement, and screendumps the
// post-click frame.
//
// The test covers three dock items (terminal, widgets, settings).
// Files is a stub in Lumen and does not emit window_opened, so it's
// intentionally skipped here.
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test dock_click_test -- --nocapture

use aegis_tests::{aegis_q35_graphical_mouse, iso, wait_for_line, AegisHarness};
use std::collections::HashMap;
use std::path::PathBuf;
use std::time::Duration;
use vortex::ConsoleStream;

/// One dock item's geometry parsed from a `[DOCK] item=... cx=... cy=...` line.
#[derive(Debug, Clone)]
struct DockItem {
    cx: i32,
    cy: i32,
}

/// Parse a single `[DOCK] item=<key> idx=<n> cx=<x> cy=<y> hw=<w> hh=<h>` line.
/// Returns `None` if the line doesn't match the expected shape.
fn parse_dock_line(line: &str) -> Option<(String, DockItem)> {
    let rest = line.strip_prefix("[DOCK] ")?;
    if rest.starts_with("ready") {
        return None;
    }
    let mut key: Option<String> = None;
    let mut cx: Option<i32> = None;
    let mut cy: Option<i32> = None;
    for token in rest.split_whitespace() {
        let (k, v) = token.split_once('=')?;
        match k {
            "item" => key = Some(v.to_string()),
            "cx" => cx = v.parse().ok(),
            "cy" => cy = v.parse().ok(),
            _ => {} // idx, hw, hh ignored for click targeting
        }
    }
    Some((key?, DockItem { cx: cx?, cy: cy? }))
}

/// Collect lines from the console stream until one containing `sentinel`
/// is seen. Returns all lines seen up to and including the sentinel.
async fn collect_until(
    stream: &mut ConsoleStream,
    sentinel: &str,
    timeout: Duration,
) -> Result<Vec<String>, String> {
    let deadline = tokio::time::Instant::now() + timeout;
    let mut collected = Vec::new();
    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                let done = line.contains(sentinel);
                collected.push(line);
                if done {
                    return Ok(collected);
                }
            }
            Ok(None) => return Err(format!("stream closed before {:?}", sentinel)),
            Err(_) => return Err(format!("timed out waiting for {:?}", sentinel)),
        }
    }
}

/// Home the cursor to (0, 0) by sending a large negative delta, then
/// move to (cx, cy) as a second delta. Splits the homing motion into
/// two hops to give Lumen time to consume PS/2 packets between them.
async fn click_at(
    proc: &vortex::QemuProcess,
    cx: i32,
    cy: i32,
) -> Result<(), String> {
    // Home in two hops (QEMU packetizes large deltas into many PS/2 packets).
    proc.mouse_move(-5000, -5000)
        .await
        .map_err(|e| format!("home1: {}", e))?;
    tokio::time::sleep(Duration::from_millis(80)).await;
    proc.mouse_move(-5000, -5000)
        .await
        .map_err(|e| format!("home2: {}", e))?;
    tokio::time::sleep(Duration::from_millis(80)).await;

    // Move to absolute target (cursor is now at 0, 0 after clamping).
    proc.mouse_move(cx, cy)
        .await
        .map_err(|e| format!("move: {}", e))?;
    tokio::time::sleep(Duration::from_millis(80)).await;

    // Press + release left button.
    proc.mouse_button(1)
        .await
        .map_err(|e| format!("press: {}", e))?;
    tokio::time::sleep(Duration::from_millis(30)).await;
    proc.mouse_button(0)
        .await
        .map_err(|e| format!("release: {}", e))?;
    Ok(())
}

#[tokio::test]
async fn dock_items_launch_apps() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_graphical_mouse(), &iso)
            .await
            .expect("QEMU failed to start");

    // Wait for Lumen to fade in.
    wait_for_line(&mut stream, "[LUMEN] ready", Duration::from_secs(30))
        .await
        .expect("[LUMEN] ready never fired within 30s");

    // Collect all [DOCK] lines up to the "[DOCK] ready" sentinel.
    let dock_lines = collect_until(&mut stream, "[DOCK] ready", Duration::from_secs(5))
        .await
        .expect("never saw [DOCK] ready sentinel");

    // Parse into key -> geometry map.
    let mut dock: HashMap<String, DockItem> = HashMap::new();
    for line in &dock_lines {
        if let Some((key, item)) = parse_dock_line(line) {
            dock.insert(key, item);
        }
    }

    // Sanity check: we expect at least terminal, widgets, settings.
    for expected in &["terminal", "widgets", "settings"] {
        assert!(
            dock.contains_key(*expected),
            "dock parser missed {}; collected lines:\n{}",
            expected,
            dock_lines.join("\n")
        );
    }

    let screenshots_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("screenshots");
    std::fs::create_dir_all(&screenshots_dir).expect("mkdir screenshots");

    // Click each expected item in turn. Cumulative state is fine for this
    // iteration — the assertion is the window_opened line plus a non-empty
    // screendump file.
    for key in ["terminal", "widgets", "settings"] {
        let item = dock.get(key).unwrap().clone();
        eprintln!("clicking {} at ({}, {})", key, item.cx, item.cy);

        click_at(&proc, item.cx, item.cy)
            .await
            .unwrap_or_else(|e| panic!("click_at {}: {}", key, e));

        wait_for_line(
            &mut stream,
            &format!("[LUMEN] window_opened={}", key),
            Duration::from_secs(5),
        )
        .await
        .unwrap_or_else(|_| {
            panic!(
                "clicked {} at ({},{}) but no window opened within 5s",
                key, item.cx, item.cy
            )
        });

        // Small settle so the window fade-in doesn't alias the capture.
        tokio::time::sleep(Duration::from_millis(500)).await;

        let out = screenshots_dir.join(format!("dock_{}.ppm", key));
        let _ = std::fs::remove_file(&out);
        proc.screendump(&out)
            .await
            .unwrap_or_else(|e| panic!("screendump {}: {}", key, e));

        tokio::time::sleep(Duration::from_millis(200)).await;

        assert!(
            out.exists(),
            "screenshot file not created at {}",
            out.display()
        );
        let sz = out.metadata().unwrap().len();
        assert!(
            sz >= 900_000,
            "screenshot for {} truncated: only {} bytes",
            key,
            sz
        );
        eprintln!("  screenshot: {} ({} bytes)", out.display(), sz);
    }

    proc.kill().await.unwrap();
}

#[cfg(test)]
mod parser_tests {
    use super::*;

    #[test]
    fn parses_valid_dock_line() {
        let line = "[DOCK] item=terminal idx=2 cx=820 cy=1020 hw=24 hh=24";
        let (key, geom) = parse_dock_line(line).expect("should parse");
        assert_eq!(key, "terminal");
        assert_eq!(geom.cx, 820);
        assert_eq!(geom.cy, 1020);
    }

    #[test]
    fn rejects_ready_sentinel() {
        let line = "[DOCK] ready";
        assert!(parse_dock_line(line).is_none());
    }

    #[test]
    fn rejects_non_dock_line() {
        let line = "[LUMEN] ready";
        assert!(parse_dock_line(line).is_none());
    }

    #[test]
    fn rejects_missing_cx() {
        let line = "[DOCK] item=terminal idx=2 cy=1020 hw=24 hh=24";
        assert!(parse_dock_line(line).is_none());
    }
}
```

- [ ] **Step 2: Run parser unit tests first (no QEMU required)**

```bash
cd /Users/dylan/Developer/aegis && cargo test --manifest-path tests/Cargo.toml --test dock_click_test parser_tests
```

Expected: 4 passed in parser_tests module. These run without a QEMU instance because they're `#[cfg(test)]` unit tests inside the integration test binary.

- [ ] **Step 3: Run the end-to-end test (requires ISO)**

On the x86 build box (see Task 6 for build instructions), once the ISO is built:

```bash
cd /Users/dylan/Developer/aegis && AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test dock_click_test dock_items_launch_apps -- --nocapture
```

Expected output includes:

```
clicking terminal at (XXX, YYY)
  screenshot: .../screenshots/dock_terminal.ppm (921615 bytes)
clicking widgets at (XXX, YYY)
  screenshot: .../screenshots/dock_widgets.ppm (921615 bytes)
clicking settings at (XXX, YYY)
  screenshot: .../screenshots/dock_settings.ppm (921615 bytes)
test dock_items_launch_apps ... ok
```

- [ ] **Step 4: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add tests/tests/dock_click_test.rs && git commit -m "$(cat <<'EOF'
test: add dock_click_test for end-to-end GUI automation

Boots Aegis graphically, scrapes dock icon coordinates from the
serial log, drives the QEMU PS/2 mouse via HMP to click each icon,
waits for [LUMEN] window_opened=<key>, and screendumps the resulting
frame. Covers terminal, widgets, and settings — Files is a stub.

Includes unit tests for the [DOCK] line parser (no QEMU required).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Verify the full suite on the x86 build box

**Files:** none — verification only.

**Context:** Per the project's session startup checklist, Aegis is built on the x86 box at `10.0.0.19` via SSH using the `aegis` key. User binaries must be rebuilt cleanly because the `make clean` target doesn't remove tracked `user/bin/*` binaries. The nuclear clean sequence is mandatory for ISO creation.

- [ ] **Step 1: Push all commits to the x86 build box**

From the dev machine:

```bash
cd /Users/dylan/Developer/aegis && git push origin master
```

And the vortex repo:

```bash
cd /Users/dylan/Developer/vortex && git push origin master 2>&1 || echo "vortex push: verify remote configured"
```

If vortex has no remote, instead rsync it:

```bash
rsync -avz --delete /Users/dylan/Developer/vortex/ 10.0.0.19:Developer/vortex/
```

- [ ] **Step 2: Nuclear clean build on the x86 box**

```bash
ssh -i ~/.ssh/aegis 10.0.0.19 'cd ~/Developer/aegis && git pull && git clean -fdx --exclude=references --exclude=.worktrees && rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf && make clean && make iso'
```

Expected: `build/aegis.iso` created cleanly with no compile errors.

- [ ] **Step 3: Run the three test files**

```bash
ssh -i ~/.ssh/aegis 10.0.0.19 'cd ~/Developer/aegis && AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test mouse_api_smoke_test --test dock_click_test -- --nocapture'
```

Expected: both tests pass. Record elapsed times.

- [ ] **Step 4: Re-run the pre-existing baseline tests to confirm no regressions**

```bash
ssh -i ~/.ssh/aegis 10.0.0.19 'cd ~/Developer/aegis && AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml -- --nocapture'
```

Expected: boot_oracle, login_flow_test, screendump_test still green. No new regressions.

- [ ] **Step 5: No commit — this task is verification only**

If any test fails, loop back to the task whose code broke it. No commit lands unless all four tests pass.

---

## Self-Review

**Spec coverage:**
- "Lumen emits dock geometry to serial" — Task 2 + Task 3 Step 1. ✅
- "Vortex gains mouse_move(x, y) and mouse_button(btn)" — Task 1. ✅
- "New test dock_click_test.rs" — Task 5. ✅
- "parse dock coords from serial" — Task 5 `parse_dock_line`. ✅
- "clicks each item" — Task 5 `click_at`. ✅
- "waits for window_opened acknowledgement" — Task 5 `wait_for_line`. ✅
- "screendumps" — Task 5 Step 1 `proc.screendump`. ✅
- "fuzzy-diffs against a golden image" — **Deferred to follow-up** (documented here). The spec's "Scope — what's NOT in this spec" lists topbar, drag, hover, right/middle click. Golden diffing wasn't explicitly deferred, but the test currently uses file-size assertion to match screendump_test.rs's pattern. This is a narrower-than-spec Phase-1 approach.
- "PS/2 mouse must be present in the QEMU config" — Task 4. ✅
- Preset name is `aegis_q35_graphical_mouse` (plan) — spec used `aegis_q35_with_mouse`. Names differ; plan name is more descriptive. Not a correctness issue.

**Gap to fix:** golden diffing is not in the plan. Adding it would extend Task 5 with a second test function that loads goldens via the existing `assert_ppm_matches` helper. However, goldens can't be checked in until the test runs once and produces stable captures — this is an inherent bootstrap problem. The pragmatic approach is to land the Phase-1 file-size test first, stabilize on hardware, then land goldens as a follow-up. Documenting this as a deliberate Phase-1 choice rather than a gap.

**Placeholder scan:** No TBDs, no "implement later", every code block is complete. The `click_at` helper contains real timings (80 ms between homing hops, 30 ms between press/release). ✅

**Type consistency:** `DockItem` struct defined in dock_click_test.rs, used consistently in the same file. `parse_dock_line` returns `Option<(String, DockItem)>`, consumed correctly by the `for` loop. `click_at` takes `&vortex::QemuProcess` — matches the type returned by `boot_stream`. ✅

**Open correctness risks I can't eliminate on paper:**
1. HMP `mouse_move` semantics (relative vs absolute) on some QEMU builds — mitigated by home-first strategy.
2. Lumen cursor clamping behavior on large negative deltas — worth a live check in Task 3 Step 3 if possible, else caught by Task 5 Step 3.
3. The 500 ms settle after `window_opened` may not be enough for a full fade-in on slow boxes — can tune up if screenshots come back aliased.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-09-gui-test-dock-click.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
