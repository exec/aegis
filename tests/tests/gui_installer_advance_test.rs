// End-to-end regression: gui-installer welcome screen advances on Enter.
//
// Pre-fix, the keystroke would silently disappear because gui-installer's
// poll() on its AF_UNIX Lumen socket was starved by the single global
// g_poll_waiter — Lumen wrote LUMEN_EV_KEY events into the ring but
// gui-installer never woke from poll() to read them. Per-fd waitqs
// (Tasks 1-10) make the wake immediate.
//
// Flow:
//   1. Boot the graphical ISO (build/aegis.iso)
//   2. Wait for [BASTION] greeter ready, log in as root
//   3. Wait for [DOCK] ready and capture the terminal icon coords
//   4. Click the terminal icon, click center to focus the window
//   5. Type `gui-installer\n` via QEMU HMP sendkey (the GUI terminal
//      consumes scancodes, not the kernel TTY) — wait for [INSTALLER] screen=1
//   6. Send Enter — assert [INSTALLER] screen=2 within 5s

use aegis_tests::{aegis_q35_graphical_mouse, iso, AegisHarness};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

async fn send_hmp(socket_path: &std::path::Path, cmd: &str) {
    let mut s = tokio::net::UnixStream::connect(socket_path).await
        .expect("connect monitor");
    let mut buf = vec![0u8; 4096];
    let _ = tokio::time::timeout(Duration::from_millis(200), s.read(&mut buf)).await;
    s.write_all(format!("{cmd}\n").as_bytes()).await.unwrap();
    let _ = tokio::time::timeout(Duration::from_millis(80), s.read(&mut buf)).await;
}

async fn home_and_move(proc: &vortex::QemuProcess, cx: i32, cy: i32) {
    for _ in 0..4 {
        proc.mouse_move(-500, -500).await.unwrap();
        tokio::time::sleep(Duration::from_millis(200)).await;
    }
    let sent_dx = (cx * 2 + 1) / 3;
    let sent_dy = (cy * 2 + 1) / 3;
    proc.mouse_move(sent_dx, sent_dy).await.unwrap();
    tokio::time::sleep(Duration::from_millis(600)).await;
}

async fn left_click(proc: &vortex::QemuProcess) {
    proc.mouse_button(1).await.unwrap();
    tokio::time::sleep(Duration::from_millis(80)).await;
    proc.mouse_button(0).await.unwrap();
    tokio::time::sleep(Duration::from_millis(80)).await;
}

#[tokio::test]
async fn enter_advances_to_disk_screen() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (mut stream, mut proc) = AegisHarness::boot_stream(
        aegis_q35_graphical_mouse(), &iso
    ).await.expect("boot");

    // ── Phase 1: drain everything until [BASTION] greeter ready ──
    let phase_deadline = tokio::time::Instant::now() + Duration::from_secs(60);
    loop {
        match tokio::time::timeout_at(phase_deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                if line.contains("[BASTION] greeter ready") { break; }
            }
            _ => panic!("bastion greeter never appeared"),
        }
    }
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys("root\tforevervigilant\n").await.unwrap();

    // ── Phase 2: wait for dock, click terminal, type gui-installer ──
    let socket_path = proc.monitor_socket_path().expect("monitor sock").to_path_buf();
    let mut term_cx = 0i32;
    let mut term_cy = 0i32;
    let mut state = "boot";
    let mut deadline = tokio::time::Instant::now() + Duration::from_secs(40);

    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                eprintln!("LINE: {line}");

                if state == "boot" && line.contains("[DOCK] item=terminal") {
                    for tok in line.split_whitespace() {
                        if let Some(rest) = tok.strip_prefix("cx=") {
                            term_cx = rest.parse().unwrap_or(0);
                        } else if let Some(rest) = tok.strip_prefix("cy=") {
                            term_cy = rest.parse().unwrap_or(0);
                        }
                    }
                }
                if state == "boot" && line.contains("[DOCK] ready") &&
                   term_cx > 0 && term_cy > 0 {
                    state = "click";
                    eprintln!(">> clicking terminal at ({term_cx}, {term_cy})");
                    home_and_move(&proc, term_cx, term_cy).await;
                    left_click(&proc).await;
                    deadline = tokio::time::Instant::now() + Duration::from_secs(20);
                }
                if state == "click" && line.contains("window_opened=terminal") {
                    state = "type";
                    tokio::time::sleep(Duration::from_millis(1000)).await;
                    // The dock click landed on the dock, not the terminal
                    // window — focus is wherever it was before. Click the
                    // center of the screen to focus the new terminal.
                    eprintln!(">> click center to focus terminal");
                    home_and_move(&proc, 320, 240).await;
                    left_click(&proc).await;
                    tokio::time::sleep(Duration::from_millis(800)).await;
                    eprintln!(">> typing gui-installer");
                    for ch in "gui-installer".chars() {
                        let key = if ch == '-' { "minus".into() }
                                  else { ch.to_string() };
                        send_hmp(&socket_path, &format!("sendkey {key}")).await;
                        tokio::time::sleep(Duration::from_millis(45)).await;
                    }
                    send_hmp(&socket_path, "sendkey ret").await;
                    deadline = tokio::time::Instant::now() + Duration::from_secs(20);
                }

                if state == "type" && line.contains("[INSTALLER] screen=1") {
                    eprintln!(">> saw screen=1 — sending Enter");
                    state = "enter";
                    // Small settle so the wizard's poll() is parked on
                    // the Lumen socket before we deliver the keystroke.
                    tokio::time::sleep(Duration::from_millis(300)).await;
                    send_hmp(&socket_path, "sendkey ret").await;
                    deadline = tokio::time::Instant::now() + Duration::from_secs(5);
                }

                if state == "enter" && line.contains("[INSTALLER] screen=2") {
                    eprintln!(">> ✓ screen=2 reached");
                    state = "done";
                    break;
                }
            }
            _ => break,
        }
    }

    let _ = proc.kill().await;

    assert_eq!(
        state, "done",
        "Enter did not advance gui-installer past welcome screen \
         (final state={state}, term_coords=({term_cx},{term_cy}))"
    );
}
