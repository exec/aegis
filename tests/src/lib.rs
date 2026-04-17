pub mod assert;
pub mod harness;
pub mod image;
pub mod presets;

pub use assert::{
    assert_boot_subsequence, assert_line_contains, assert_no_line_contains,
    assert_subsystem_fail, assert_subsystem_ok, wait_for_line, WaitTimeout,
};
pub use harness::{AegisHarness, HarnessError};
pub use image::{assert_ppm_matches, compare_ppm, load_ppm, Ppm, PpmDiff};
pub use presets::{
    aegis_arm64_virt, aegis_pc, aegis_q35, aegis_q35_graphical_mouse,
    aegis_q35_gui_installer, aegis_q35_installer, aegis_q35_installer_4k,
    aegis_q35_installed_ovmf, aegis_q35_installed_ovmf_4k,
    arm64_elf, disk, iso,
};
pub use vortex::{ConsoleStream, QemuProcess};
