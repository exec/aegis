// End-to-end GUI test: boots Aegis graphically, scrapes dock icon
// coordinates from the serial log, drives the QEMU PS/2 mouse via HMP
// to click each testable icon, waits for the corresponding
// [LUMEN] window_opened=<key> acknowledgement, and screendumps the
// post-click frame.
//
// Current HEAD scope: terminal and widgets only. Settings is a stub
// in HEAD (open_settings lives in unlanded Phase 47 WIP) and Files is
// a permanent stub, so neither emits window_opened. Once Phase 47
// lands, extend TESTED_ITEMS below to include "settings".
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test dock_click_test -- --nocapture

use aegis_tests::{aegis_q35_graphical_mouse, iso, wait_for_line, AegisHarness};
use std::collections::HashMap;
use std::path::PathBuf;
use std::time::Duration;
use vortex::ConsoleStream;

/// Dock item keys that are expected to emit [LUMEN] window_opened=<key>
/// when clicked on the current HEAD. Settings and Files are stubs and
/// are intentionally excluded.
const TESTED_ITEMS: &[&str] = &["terminal", "widgets"];

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

/// Home the cursor to (0, 0) then move to (cx, cy).
///
/// Lumen's compositor applies a **1.5× speed multiplier** to all
/// mouse deltas via `cursor_x += dx + dx / 2` (see
/// `user/bin/lumen/compositor.c:488`). To land the cursor at an
/// absolute (cx, cy), we must send 2/3 of the desired delta so
/// Lumen's 1.5× scaling brings it back to (cx, cy).
///
/// QEMU's HMP `mouse_move` also delivers deltas to the guest as PS/2
/// mouse packets, and the guest needs time between packets to
/// consume them. Homing uses small hops with settles between each.
async fn home_and_move(
    proc: &vortex::QemuProcess,
    cx: i32,
    cy: i32,
) -> Result<(), String> {
    // Home in small hops. We deliberately send deltas larger than
    // the screen divided by 1.5 so that after Lumen's scaling the
    // cursor clamps to (0, 0) regardless of starting position.
    for _ in 0..3 {
        proc.mouse_move(-700, -500)
            .await
            .map_err(|e| format!("home: {}", e))?;
        tokio::time::sleep(Duration::from_millis(200)).await;
    }

    // Compensate for Lumen's 1.5× scaling: send 2/3 of the target
    // so the cursor lands at (cx, cy) after scaling. Using
    // `(x * 2 + 1) / 3` rounds to nearest rather than toward zero.
    let sent_dx = (cx * 2 + 1) / 3;
    let sent_dy = (cy * 2 + 1) / 3;
    proc.mouse_move(sent_dx, sent_dy)
        .await
        .map_err(|e| format!("move: {}", e))?;
    tokio::time::sleep(Duration::from_millis(600)).await;
    Ok(())
}

/// Press + release left button with a visible gap so Lumen sees a
/// discrete click, not a flutter.
async fn left_click(proc: &vortex::QemuProcess) -> Result<(), String> {
    proc.mouse_button(1)
        .await
        .map_err(|e| format!("press: {}", e))?;
    tokio::time::sleep(Duration::from_millis(80)).await;
    proc.mouse_button(0)
        .await
        .map_err(|e| format!("release: {}", e))?;
    tokio::time::sleep(Duration::from_millis(80)).await;
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

    // Bastion owns the graphical session and ships a login greeter.
    // We must authenticate before Lumen is spawned.
    wait_for_line(&mut stream, "[BASTION] greeter ready", Duration::from_secs(30))
        .await
        .expect("[BASTION] greeter ready never fired within 30s");
    tokio::time::sleep(Duration::from_millis(200)).await;

    // Type root<Tab>forevervigilant<Enter> — same credentials as
    // login_flow_test. This routes to the PS/2 keyboard because the
    // preset does not add usb-kbd.
    proc.send_keys("root\tforevervigilant\n")
        .await
        .expect("send_keys failed");
    eprintln!("sent: root<Tab>forevervigilant<Enter>");

    // Wait for Lumen to finish fading in.
    wait_for_line(&mut stream, "[LUMEN] ready", Duration::from_secs(20))
        .await
        .expect("[LUMEN] ready never fired within 20s");

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

    // Sanity check: we expect every item in TESTED_ITEMS to be in the
    // parsed map. Other dock items (settings, files) may also be present
    // but we don't click them in HEAD.
    for expected in TESTED_ITEMS {
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

    // Click each tested item in turn. Cumulative state is fine — the
    // assertion is the window_opened line plus a non-empty screendump.
    for key in TESTED_ITEMS {
        let item = dock.get(*key).unwrap().clone();
        eprintln!("clicking {} at ({}, {})", key, item.cx, item.cy);

        // Phase 1: home and move to target.
        home_and_move(&proc, item.cx, item.cy)
            .await
            .unwrap_or_else(|e| panic!("home_and_move {}: {}", key, e));

        // Diagnostic: capture a frame after the cursor move but
        // BEFORE the click, so we can see where the cursor actually
        // landed.
        let premove = screenshots_dir.join(format!("dock_{}_premove.ppm", key));
        let _ = std::fs::remove_file(&premove);
        if let Err(e) = proc.screendump(&premove).await {
            eprintln!("  premove screendump failed: {}", e);
        } else {
            eprintln!("  premove screenshot: {}", premove.display());
        }

        // Phase 2: click.
        left_click(&proc)
            .await
            .unwrap_or_else(|e| panic!("left_click {}: {}", key, e));

        // Diagnostic: capture a frame immediately after the click
        // dispatch so we can see whether any visible change happened.
        let diag = screenshots_dir.join(format!("dock_{}_postclick.ppm", key));
        let _ = std::fs::remove_file(&diag);
        if let Err(e) = proc.screendump(&diag).await {
            eprintln!("  postclick screendump failed: {}", e);
        } else {
            eprintln!("  postclick screenshot: {}", diag.display());
        }

        // Wait for window_opened sentinel, but collect every line
        // seen so we can print a diagnostic trace on failure.
        let sentinel = format!("[LUMEN] window_opened={}", key);
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        let mut trace: Vec<String> = Vec::new();
        let mut found = false;
        while tokio::time::Instant::now() < deadline {
            match tokio::time::timeout_at(deadline, stream.next_line()).await {
                Ok(Some(line)) => {
                    let done = line.contains(&sentinel);
                    trace.push(line);
                    if done {
                        found = true;
                        break;
                    }
                }
                Ok(None) | Err(_) => break,
            }
        }
        if !found {
            eprintln!("--- serial trace while waiting for {} ---", sentinel);
            for l in &trace {
                eprintln!("  {}", l);
            }
            eprintln!("--- end trace ({} lines) ---", trace.len());
            panic!(
                "clicked {} at ({},{}) but no window opened within 5s (see {})",
                key, item.cx, item.cy,
                diag.display()
            );
        }

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
