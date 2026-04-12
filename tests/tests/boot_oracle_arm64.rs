// tests/tests/boot_oracle_arm64.rs
//
// ARM64 boot oracle — requires a built ARM64 kernel ELF and
// qemu-system-aarch64 on PATH.
//
// Run locally:
//   make -C kernel/arch/arm64
//   VORTEX_QEMU_BINARY=qemu-system-aarch64 \
//   AEGIS_ARM64_ELF=$PWD/kernel/arch/arm64/build/aegis-arm64.elf \
//   AEGIS_BOOT_TIMEOUT=10 \
//     cargo test --manifest-path tests/Cargo.toml \
//     --test boot_oracle_arm64 -- --nocapture
//
// The ARM64 kernel currently has no way to signal "test done" — after
// `[SCHED] OK` it runs the idle task forever. The harness uses
// AEGIS_BOOT_TIMEOUT (default 30s) to kill QEMU once serial capture is
// done. A 10s deadline is a comfortable margin: on HVF the boot is
// sub-second, on software QEMU it's typically 3-4s.
//
// TODO(phase A4): once an ARM64 userland exists, replace the timeout
// trigger with an ARM semihosting exit (`-semihosting-config
// enable=on,target=native` + `HLT #0xF000` from the kernel at
// shutdown). That's the future-correct answer, matching x86
// isa-debug-exit semantics.

use aegis_tests::{
    aegis_arm64_virt, arm64_elf, assert_boot_subsequence, AegisHarness,
};

/// Subsequence from `ARM64.md` §15 — the honest current boot trace.
/// Extra output is allowed between these lines; their relative order
/// is enforced. When the port adds new subsystems this list grows.
const BOOT_ORACLE_ARM64: &[&str] = &[
    "[SERIAL] OK: PL011 UART initialized",
    "[PMM] OK:",                                 // "[PMM] OK: 2048MB usable across 1 regions"
    "[VMM] OK: ARM64 4KB-granule page tables active",
    "[VMM] OK: mapped-window allocator active",
    "[KVA] OK: kernel virtual allocator active",
    "[GIC] OK: GICv2 initialized",
    "[TIMER] OK: ARM generic timer at 100 Hz",
    "[CAP] OK: capability subsystem initialized",
    "[RNG] OK: ChaCha20 CSPRNG seeded",
    "[VFS] OK: initialized",
    "[INITRD] OK:",                              // "[INITRD] OK: 26 files registered"
    "[INIT] ARM64 userland not yet built",
    "[SCHED] OK: scheduler started, 1 tasks",
];

#[tokio::test]
async fn arm64_boot_oracle() {
    let elf = arm64_elf();
    if !elf.exists() {
        eprintln!(
            "SKIP: {} not found — run `make -C kernel/arch/arm64` first",
            elf.display()
        );
        return;
    }

    // Tell Vortex to spawn qemu-system-aarch64 instead of the x86
    // default. QemuBackend::new() reads VORTEX_QEMU_BINARY once at
    // construction, so this env var must be set before boot_kernel()
    // calls QemuBackend::new() internally. Each file in tests/tests/
    // is compiled as a separate test binary by cargo, so the env var
    // set here does not leak into the x86 boot oracle test.
    //
    // SAFETY: test-only, no threads touching this env before we set it.
    unsafe {
        std::env::set_var("VORTEX_QEMU_BINARY", "qemu-system-aarch64");
    }

    // Default AEGIS_BOOT_TIMEOUT is 30s (see harness.rs::boot_timeout).
    // We could drop that here via set_var("AEGIS_BOOT_TIMEOUT", "10"),
    // but 30s is already fine on CI and leaves headroom for slow
    // software QEMU runs. Callers that want a tighter budget can
    // export AEGIS_BOOT_TIMEOUT themselves.

    let out = AegisHarness::boot_kernel(aegis_arm64_virt(), &elf)
        .await
        .expect("ARM64 kernel boot failed");

    assert_boot_subsequence(&out, BOOT_ORACLE_ARM64);
}
