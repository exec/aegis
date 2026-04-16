// Regression test for Lumen proxy-window use-after-free on client
// disconnect.
//
// Pre-fix bug: lumen_server_hangup and handle_destroy_window each
// called glyph_window_destroy(pw->win) AFTER comp_remove_window had
// already freed it. Subsequent compositor activity (mouse moves,
// clicks) then dereferenced corrupted heap state and Lumen page-
// faulted in user mode.
//
// Test flow: boot graphical → log in → wait for [PROBE] PASS (probe
// has connected, created window, presented, destroyed, exited) → drive
// mouse via HMP for several seconds → assert Lumen never produced a
// page-fault marker on serial.
//
// Requires the lumen-probe vigil oneshot service to be present in
// rootfs/etc/vigil/services/lumen-probe/ so the probe auto-launches.
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml \
//          --test lumen_proxy_uaf_test -- --nocapture

use aegis_tests::{aegis_q35_graphical_mouse, iso, wait_for_line, AegisHarness};
use std::time::Duration;

#[tokio::test]
async fn proxy_window_disconnect_does_not_uaf() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_graphical_mouse(), &iso)
            .await
            .expect("QEMU failed to start");

    wait_for_line(&mut stream, "[BASTION] greeter ready", Duration::from_secs(60))
        .await
        .expect("bastion greeter never appeared");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys("root\tforevervigilant\n").await.unwrap();

    wait_for_line(&mut stream, "[LUMEN] ready", Duration::from_secs(30))
        .await
        .expect("lumen never became ready");

    // Wait for the probe to complete its full lifecycle:
    // connect → create window → present → destroy → exit. The probe
    // emits [PROBE] PASS just before sleeping 2s and exiting.
    wait_for_line(&mut stream, "[PROBE] PASS", Duration::from_secs(60))
        .await
        .expect("probe never reached PASS");

    // Probe sleeps 2s then exits; allow extra slack for kernel cleanup
    // (close fds, send EOF, lumen_server_hangup runs in next tick).
    tokio::time::sleep(Duration::from_secs(4)).await;

    // Drive mouse interactions to exercise comp_handle_mouse paths
    // that previously dereferenced the freed proxy window.
    for i in 0..6 {
        let dx = if i % 2 == 0 { 80 } else { -80 };
        let dy = if i % 2 == 0 { 50 } else { -50 };
        proc.mouse_move(dx, dy)
            .await
            .unwrap_or_else(|e| panic!("mouse_move iter {}: {}", i, e));
        tokio::time::sleep(Duration::from_millis(150)).await;
        proc.mouse_button(1)
            .await
            .unwrap_or_else(|e| panic!("press iter {}: {}", i, e));
        tokio::time::sleep(Duration::from_millis(80)).await;
        proc.mouse_button(0)
            .await
            .unwrap_or_else(|e| panic!("release iter {}: {}", i, e));
        tokio::time::sleep(Duration::from_millis(150)).await;
    }

    // Drain remaining serial for ~2s and look for the page-fault
    // marker the kernel emits on user-mode exceptions.
    let drain_deadline = tokio::time::Instant::now() + Duration::from_secs(3);
    let mut saw_fault = false;
    let mut fault_line = String::new();
    while tokio::time::Instant::now() < drain_deadline {
        match tokio::time::timeout(Duration::from_millis(300), stream.next_line()).await {
            Ok(Some(line)) => {
                // Aegis prints "[PANIC]" for kernel panics and
                // "user fault" / "EXCEPTION" / "page fault" for ring-3
                // faults via the kernel's panic_screen / isr_dispatch
                // user-mode path. Catch any of them.
                // isr_dispatch prints "[PANIC] exception N at RIP=..."
                // for any CPU exception (including user-mode #PF).
                if line.contains("[PANIC]") {
                    saw_fault = true;
                    fault_line = line.clone();
                    break;
                }
            }
            _ => {}
        }
    }

    let _ = proc.kill().await;

    assert!(
        !saw_fault,
        "Lumen produced a fault marker after probe disconnect: {}",
        fault_line
    );
}
