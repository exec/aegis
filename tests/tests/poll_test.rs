// tests/tests/poll_test.rs
//
// Integration test — boots QEMU, verifies kernel poll self-test in boot oracle.
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml poll_test

use aegis_tests::{aegis_pc, iso, AegisHarness, assert_subsystem_ok};

#[tokio::test]
async fn poll_boot_oracle() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found — run `make iso` first", iso.display());
        return;
    }
    let out = AegisHarness::boot(aegis_pc(), &iso)
        .await
        .expect("QEMU failed to start");
    assert_subsystem_ok(&out, "POLL");
}
