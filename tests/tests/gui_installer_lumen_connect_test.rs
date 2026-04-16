// Reproduce: lumen_connect failed (-5) when launching gui-installer
// from a Lumen terminal. Boot graphical, log in, click the dock
// terminal icon, type `gui-installer\n`, and capture diagnostic
// markers. Every serial line is printed so we don't lose evidence.

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
async fn launch_gui_installer_from_dock_terminal() {
    let iso = iso();
    if !iso.exists() { eprintln!("SKIP: {} not found", iso.display()); return; }

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

    // ── Phase 2: dump everything from now on ──
    let socket_path = proc.monitor_socket_path().expect("monitor sock").to_path_buf();
    let mut all: Vec<String> = Vec::new();
    let mut term_cx = 0i32;
    let mut term_cy = 0i32;
    let mut state = "boot";
    let mut sent_keys = false;
    let mut deadline = tokio::time::Instant::now() + Duration::from_secs(40);

    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                eprintln!("LINE: {line}");
                all.push(line.clone());

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
                    // Click in the middle of the screen to focus the new
                    // terminal window. The dock click happened at y=430
                    // which is BELOW the terminal (which spans y≈100..380),
                    // so the original click landed on empty desktop and left
                    // focus on whatever was previously focused.
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
                    sent_keys = true;
                    deadline = tokio::time::Instant::now() + Duration::from_secs(15);
                }

                // Success markers
                if line.contains("[INSTALLER] screen=1") ||
                   line.contains("[LUMEN-CLI]") ||
                   line.contains("gui-installer:") {
                    // Keep collecting for a bit more then break
                    deadline = tokio::time::Instant::now() + Duration::from_secs(2);
                }
            }
            _ => break,
        }
    }

    let _ = proc.kill().await;

    eprintln!("=== summary ===");
    eprintln!("sent_keys: {sent_keys}");
    eprintln!("term coords: ({term_cx}, {term_cy})");

    let saw_connect_diag = all.iter().any(|l| l.contains("[LUMEN-CLI]"));
    let saw_installer = all.iter().any(|l| l.contains("[INSTALLER] screen="));
    let saw_connect_fail = all.iter().any(|l|
        l.contains("gui-installer: lumen_connect failed"));

    if saw_installer {
        eprintln!("✓ gui-installer started, lumen_connect succeeded");
    } else if saw_connect_diag || saw_connect_fail {
        let diag: Vec<&String> = all.iter().filter(|l|
            l.contains("LUMEN-CLI") || l.contains("LUMEN-SRV") ||
            l.contains("gui-installer:")).collect();
        panic!("lumen_connect diagnostics:\n{}",
               diag.iter().map(|s| s.as_str()).collect::<Vec<_>>().join("\n"));
    } else {
        panic!("no diagnostic markers — keys may not have reached stsh");
    }
}
