// tests/tests/boot_oracle.rs
//
// Integration test — requires a built ISO.
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml boot_oracle

use aegis_tests::{aegis_pc, iso, AegisHarness, assert_boot_subsequence, assert_no_line_contains};

/// Lines from tests/historical/expected/boot.txt expressed as a subsequence.
/// Gaps are allowed (extra output is fine); order is enforced.
const BOOT_ORACLE: &[&str] = &[
    "[SERIAL] OK: COM1 initialized",
    "[VGA] OK: text mode",
    "[PMM] OK:",
    "[VMM] OK: kernel mapped",
    "[VMM] OK: mapped-window allocator",
    "[KVA] OK:",
    "[CAP] OK: capability subsystem",
    "[IDT] OK:",
    "[PIC] OK:",
    "[PIT] OK:",
    "[KBD] OK:",
    "[MOUSE] OK:",
    "[GDT] OK:",
    "[TSS] OK:",
    "[SYSCALL] OK:",
    "[SMAP] OK:",
    "[SMEP] OK:",
    "[RNG] OK:",
    "[RAMDISK] OK:",
    "[VFS] OK:",
    "[INITRD] OK:",
    "[ACPI] OK:",
    "[LAPIC] OK:",
    "[IOAPIC] OK:",
    "[PCIE] OK:",
    "[EXT2] OK: mounted ramdisk0",
    "[CAP_POLICY] OK:",
    "[POLL] OK:",
    "[SMP] OK:",
    "[CAP] OK: 7 baseline capabilities",
    "[VMM] OK: identity map removed",
    "[SCHED] OK:",
];

#[tokio::test]
async fn boot_oracle() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found — run `make iso` first", iso.display());
        return;
    }
    let out = AegisHarness::boot(aegis_pc(), &iso)
        .await
        .expect("QEMU failed to start");
    assert_boot_subsequence(&out, BOOT_ORACLE);
}

#[tokio::test]
async fn no_net_lines_on_pc() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found — run `make iso` first", iso.display());
        return;
    }
    let out = AegisHarness::boot(aegis_pc(), &iso)
        .await
        .expect("QEMU failed to start");
    // -machine pc has no virtio-net; net init lines must not appear
    assert_no_line_contains(&out, "[NET] configured:");
    assert_no_line_contains(&out, "[NET] ICMP:");
}
