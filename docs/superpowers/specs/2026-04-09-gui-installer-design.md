# GUI Installer Design

**Date:** 2026-04-09
**Status:** Draft
**Phase:** 47

## Problem

Aegis has a text-mode installer at `user/bin/installer/main.c` (748 lines). It partitions NVMe, copies rootfs from ramdisk, installs EFI GRUB, sets up a user account. It works. But:

1. **No active test coverage.** `tests/historical/test_installer.py` exists but is archived Python; the Rust cargo harness does not run it. Any regression in the installer path is silently uncaught by `make test`.
2. **Phase 47 calls for a graphical installer** matching Bastion's look so a newcomer booting an Aegis USB sees a real GUI and not a terminal prompt.
3. **No logic sharing.** A second installer written alongside the first would duplicate GPT writing, CRC32, ext2 copy, etc. That's ~300 LOC of duplication and two places to fix any bug.

## Approach

Three sequential pieces, test-first:

1. **`installer_test.rs`** — port `tests/historical/test_installer.py` to the Rust cargo harness as a two-boot regression gate. Runs against the **current text-mode installer** as written, verifying the install flow still works end to end before any refactoring begins. Land this first as a pure safety net.
2. **`user/lib/libinstall/`** — static library containing all the pure logic currently sitting in `user/bin/installer/main.c`. Progress callbacks replace inline `printf`. Both the text installer and the GUI installer link against it. Re-run `installer_test` to prove the refactor didn't regress.
3. **`user/bin/gui-installer/`** — Glyph-based graphical installer wizard. Links libinstall.a for the same install logic the TUI uses. Runs as the default installer binary on the live ISO. Keyboard-driven (Tab / Enter / shortcuts) so the test harness can drive it via HMP `sendkey` exactly like `login_flow_test` drives Bastion — no mouse coordinate math required. A second test (`gui_installer_test.rs`) exercises this path.

Test → refactor → rewrite. Each step is gated by green tests.

**Why keyboard-first:** HMP `sendkey` is rock-solid. HMP `mouse_move` requires accounting for Lumen's 1.5× speed multiplier, QEMU's ~635-unit-per-poll drain limit, and careful sequencing (we proved that out in dock_click_test, but it was an iteration). Keeping the installer keyboard-driven means the test is a direct clone of `login_flow_test`'s pattern.

## libinstall API

Header: `user/lib/libinstall/libinstall.h`

```c
#ifndef LIBINSTALL_H
#define LIBINSTALL_H

#include <stdint.h>

/* Progress callback. Called by libinstall during long-running operations.
 * All callbacks are optional (nullable).
 *
 *   on_step     — called at the start of each named phase
 *                 ("Writing GPT", "Copying rootfs", "Installing ESP", ...).
 *   on_progress — called while a phase is running, value 0..100.
 *   on_error    — called with a human-readable message if any step fails.
 *                 libinstall always returns -1 from the failing call; the
 *                 error callback is for UI display only.
 *
 * ctx is an opaque caller pointer passed back to every callback. */
typedef struct {
    void (*on_step)(const char *label, void *ctx);
    void (*on_progress)(int pct, void *ctx);
    void (*on_error)(const char *msg, void *ctx);
    void *ctx;
} install_progress_t;

/* Block device enumeration. Populates `out` with up to `max` devices and
 * returns the number found. A device's name is the same string the kernel
 * uses (e.g. "nvme0", "ramdisk0"). */
typedef struct {
    char     name[16];
    uint64_t block_count;
    uint32_t block_size;
} install_blkdev_t;

int install_list_blkdevs(install_blkdev_t *out, int max);

/* Write a fresh protective MBR + GPT to `devname`. Creates two
 * partitions: a 32 MB EFI System Partition and an Aegis root partition
 * covering the rest of the disk. Returns 0 on success. */
int install_write_gpt(const char *devname, uint64_t disk_blocks,
                      install_progress_t *p);

/* Ask the kernel to re-read the partition table on `devname`. Returns
 * the number of partitions found (>0 on success). */
int install_rescan_gpt(const char *devname);

/* Copy the `ramdisk1` block device (ESP image) to the ESP partition
 * on `devname`. */
int install_copy_esp(const char *devname, install_progress_t *p);

/* Copy the `ramdisk0` block device (ext2 rootfs image) to `dst_dev`.
 * Emits progress callbacks every ~10% of bytes copied. */
int install_copy_rootfs(const char *dst_dev, uint64_t dst_blocks,
                        install_progress_t *p);

/* Write a fresh grub.cfg to /boot/grub/grub.cfg on the CURRENTLY
 * MOUNTED root (i.e. the ramdisk0 ext2 before it gets copied to the
 * target disk). Must be called before install_copy_rootfs. */
int install_write_grub_cfg(void);

/* Delete test binaries from the currently mounted rootfs. Called
 * before install_copy_rootfs so the installed system doesn't ship
 * with thread_test, mmap_test, etc. */
void install_strip_test_binaries(void);

/* Hash a password using crypt(3) with a fresh SHA-512 salt.
 * `out` must be at least 128 bytes. Returns 0 on success. */
int install_hash_password(const char *password, char *out, int outsz);

/* Write /etc/passwd, /etc/shadow, and /etc/group on the currently
 * mounted rootfs. `username` and `user_hash` may be NULL to skip
 * the optional non-root user. `root_hash` is required. */
int install_write_credentials(const char *root_hash,
                              const char *username,
                              const char *user_hash);

/* One-shot driver: run every phase in order using the supplied
 * progress callbacks and credentials. Used by BOTH installers —
 * the UI layer is responsible for collecting the devname and
 * credentials, then handing them off to this function.
 *
 * Phases in order:
 *   1. write_grub_cfg
 *   2. strip_test_binaries
 *   3. write_credentials (using the hashes the caller precomputed)
 *   4. write_gpt
 *   5. rescan_gpt
 *   6. copy_rootfs (to the rescanned root partition)
 *   7. copy_esp
 *   8. sync()
 *
 * Returns 0 on success, -1 on any failure (error callback called). */
int install_run_all(const char *devname, uint64_t disk_blocks,
                    const char *root_hash,
                    const char *username,
                    const char *user_hash,
                    install_progress_t *p);

#endif /* LIBINSTALL_H */
```

The existing `user/bin/installer/main.c` shrinks to ~150 lines: read/confirm disk, prompt credentials via `read_line`/`read_password`, hash, call `install_run_all` with a progress struct whose callbacks just `printf`. No behavior change.

## GUI installer — wizard layout

Five sequential screens, one window, keyboard-driven. The window has a fixed size (640×480 centered on screen) with a dark frame, title bar, and a content area. A status bar at the bottom shows the current step. Tab cycles focus. Enter advances. Escape backs up (except on the last confirmation screen where Escape aborts).

```
┌─────────────────────────────────────────────────────────┐
│                Aegis Installer                          │
├─────────────────────────────────────────────────────────┤
│                                                         │
│                  < screen content >                     │
│                                                         │
│                                                         │
├─────────────────────────────────────────────────────────┤
│  Step N of 5                          [ Back ] [ Next ] │
└─────────────────────────────────────────────────────────┘
```

### Screen 1 — Welcome

- Title label: "Welcome to Aegis"
- Body text: "This installer will install Aegis to your disk. All data on the target disk will be erased."
- Single button: `[ Next ]` (keyboard focus on load).
- `[LUMEN] installer ready` emitted to stderr on first paint — this is the test harness's `wait_for_line` marker (matches Bastion's `[BASTION] greeter ready` pattern).

### Screen 2 — Disk selection

- Title label: "Select target disk"
- ListView of candidate block devices: `install_list_blkdevs()` minus ramdisk entries and minus anything containing a `p` in its name (that's a partition). Each row shows `name (size MB)`.
- If only one candidate, it's preselected and the listview is read-only.
- Buttons: `[ Back ]` / `[ Next ]`. Next disabled until a disk is selected.

### Screen 3 — User account

Two grouped sections:
- **Root password** (required): two textfield widgets, password-masked (`mask_char='*'`). Labeled "Root password" and "Confirm root password".
- **User account** (optional): textfield for username, two masked textfields for password + confirm. Label: "Optional user account (leave username blank to skip)".

Validation on `[ Next ]`:
- Root password non-empty and matches confirm.
- If username non-empty, user password matches confirm.
- On failure, show an inline red error label directly below the offending field. Keep focus on the field.

### Screen 4 — Confirm

- Title label: "Ready to install"
- Summary text:
  - Target disk: `nvme0`
  - Root password: set
  - User account: `alice` (or "none")
- Buttons: `[ Back ]` / `[ Install ]` (the destructive action button; accent-colored, focused by default).

### Screen 5 — Progress

- Title label: dynamic, updated via `on_step` callback ("Writing partition table", "Copying rootfs", etc.).
- `glyph_progress` bar, 0–100, updated via `on_progress`.
- Log area: scrolling textview showing the last ~8 lines of step labels for context.
- No buttons visible during the run.
- On success: title → "Installation complete", single `[ Reboot ]` button focused.
- On failure: title → "Installation failed", red error text below the progress bar, buttons `[ Retry ]` / `[ Abort ]`.

### Progress callback wiring

```c
static void gui_on_step(const char *label, void *ctx) {
    gui_state_t *st = ctx;
    glyph_label_set_text(st->step_label, label);
    gui_log_append(st, label);
    glyph_progress_set_value(st->bar, 0);
    /* Serial marker for the test harness. */
    dprintf(2, "[INSTALLER] step=%s\n", label);
    glyph_window_mark_all_dirty(st->win);
}
static void gui_on_progress(int pct, void *ctx) {
    gui_state_t *st = ctx;
    glyph_progress_set_value(st->bar, pct);
    /* No serial spam on every 1% — only log round numbers. */
    if (pct % 10 == 0)
        dprintf(2, "[INSTALLER] progress=%d\n", pct);
}
static void gui_on_error(const char *msg, void *ctx) {
    gui_state_t *st = ctx;
    st->error_msg = strdup(msg);
    st->phase = GUI_PHASE_ERROR;
    dprintf(2, "[INSTALLER] error=%s\n", msg);
    glyph_window_mark_all_dirty(st->win);
}
```

**Serial markers emitted by gui-installer:**
- `[LUMEN] installer ready` — first paint of screen 1
- `[INSTALLER] screen=<n>` — entering screen n
- `[INSTALLER] step=<label>` — each libinstall phase
- `[INSTALLER] progress=<pct>` — round-10 percent ticks
- `[INSTALLER] done` — successful completion
- `[INSTALLER] error=<msg>` — any failure

The test harness scrapes these for checkpoints.

## Testing strategy

Two new tests, both under `tests/tests/`:

### `installer_test.rs` — CLI installer regression gate (Phase 1)

Direct port of `test_installer.py` to the Rust harness. Uses a **persistent NVMe disk image** that survives between the two QEMU boots.

**Flow:**
1. Create fresh disk image `tests/target/installer_test_disk.img` (128 MB, sparse).
2. **Boot 1** — live ISO with empty NVMe attached:
   - Wait for `[BASTION] greeter ready`.
   - Send `root<Tab>forevervigilant<Enter>`.
   - Wait for `[LUMEN] ready`.
   - Launch the installer through stsh (TODO: installer doesn't currently auto-launch — see "Open questions" below).
   - Type `y<Enter>` to confirm disk.
   - Type root password + confirm.
   - Type username (or empty) + optional password.
   - Wait for `[INSTALLER] done` or the text marker `=== Installation complete! ===`.
   - Gracefully shut down via `sys_reboot(0)`.
3. **Boot 2** — no ISO, just the NVMe disk image:
   - Wait for `[BASTION] greeter ready` (proves boot from disk works).
   - Send the credentials used in boot 1.
   - Wait for `[LUMEN] ready` (proves login works).
   - Assert no `[EXT2] FAIL` or `[GPT] FAIL` in the captured serial.
   - Shut down.
4. Leave the disk image in place for debugging; next run overwrites.

**Harness support needed:** add `AegisHarness::boot_with_disk(opts, iso, disk_path)` that passes the disk as an `-hda` device in addition to (or instead of) the ISO. Currently `boot` / `boot_stream` only accept an ISO parameter. The preset gains a new boolean `drive_disk` flag and the harness appends `-drive file=<path>,format=raw,if=none,id=nvme0 -device nvme,drive=nvme0,serial=aegis0`.

**Timeout:** boot 1 ~90 s (60 s of install + normal boot + login), boot 2 ~40 s. Budget 180 s total via an `AEGIS_INSTALLER_TEST_TIMEOUT` env var (default 180).

### `gui_installer_test.rs` — GUI installer regression gate (Phase 3)

Same two-boot structure, but boot 1 drives the GUI installer instead of the text installer:

1. Fresh disk image.
2. Boot 1 — live ISO. If the live ISO's init is set to launch `gui-installer` directly (the intent for the installer USB), wait for `[LUMEN] installer ready`. Otherwise launch through stsh like the CLI test.
3. Drive the wizard via `send_keys`:
   - Screen 1 (Welcome): `Enter` → Screen 2.
   - Screen 2 (Disk): `Enter` (single disk auto-selected) → Screen 3.
   - Screen 3 (User): `forevervigilant<Tab>forevervigilant<Tab><Tab>alice<Tab>alicepass<Tab>alicepass<Enter>` → Screen 4.
   - Screen 4 (Confirm): `Enter` (Install button is default-focused) → Screen 5.
   - Screen 5 (Progress): wait for `[INSTALLER] done`, then `Enter` on the Reboot button.
4. Boot 2 — same as the CLI test: verify boot from disk + login works.

Asserts along the way:
- `[INSTALLER] screen=1` … `screen=5` appear in order.
- `[INSTALLER] step=...` markers include each expected libinstall phase.
- `[INSTALLER] progress=100` appears at least once.
- `[INSTALLER] done` appears.
- Boot 2 login succeeds.

## File structure

**Create:**
- `user/lib/libinstall/libinstall.h` — public API
- `user/lib/libinstall/gpt.c` — `write_gpt`, `rescan_gpt`, CRC32, GPT structures
- `user/lib/libinstall/copy.c` — `copy_blocks`, `copy_rootfs`, `copy_esp`, `list_blkdevs`
- `user/lib/libinstall/config.c` — `write_grub_cfg`, `strip_test_binaries`
- `user/lib/libinstall/credentials.c` — `hash_password`, `write_credentials`
- `user/lib/libinstall/run.c` — `install_run_all` (the orchestration driver)
- `user/lib/libinstall/Makefile` — builds libinstall.a, mirrors libglyph/Makefile
- `user/bin/gui-installer/main.c` — the wizard (~800 LOC estimate)
- `user/bin/gui-installer/Makefile` — links libinstall + libglyph + libcitadel + libauth + -lcrypt
- `tests/tests/installer_test.rs` — CLI installer two-boot regression test
- `tests/tests/gui_installer_test.rs` — GUI installer two-boot regression test

**Modify:**
- `user/bin/installer/main.c` — shrink from 748 LOC to ~150 LOC, thin TUI over libinstall
- `user/bin/installer/Makefile` — link against libinstall.a
- `Makefile` — add libinstall + gui-installer to the build
- `tests/src/harness.rs` — add `boot_with_disk` helper + persistent disk handling
- `tests/src/presets.rs` — add `aegis_q35_with_disk(path)` preset builder
- `tests/src/lib.rs` — re-export new helpers
- `rootfs/etc/aegis/caps.d/gui-installer` (new file) — same caps as the text installer (`admin DISK_ADMIN AUTH`)

**No changes to:** kernel, glyph widgets, bastion, lumen, citadel, libauth. The gui-installer is a pure userspace addition.

## Scope — what's NOT in this spec

- Multi-disk selection UI beyond a simple listview (target is single-disk auto-pick for now).
- Disk partitioning customization (swap partition, home partition, LVM, encryption). The layout is fixed: 32 MB ESP + rest as Aegis root, same as the text installer.
- Locale/keymap selection.
- Network setup during install.
- Package selection or metapackages.
- Dark/light theme toggle (the runtime theme refactor is Phase 47+ work — see notes on the abandoned theme WIP).
- Uninstaller or repair mode.
- Progress animations or transitions between wizard screens (static redraw is fine).
- Mouse-driven wizard navigation (keyboard is required for test reliability; mouse is a nice-to-have follow-up once the wizard is keyboard-stable).
- Rewriting or extending `libinstall.h` beyond what this spec lists. Additional helpers can land as follow-up patches.

## Open questions

1. **How does the test reach the installer binary?** The text installer is a CLI command at `/bin/installer` that the user runs from stsh. For the test, we need to either (a) auto-launch the installer from init when booted in "installer mode" (a new vigil service gated on a kernel cmdline flag), or (b) drive stsh through to execute `installer`. The second is simpler — the existing `login_flow_test` proves we can drive stsh. The first is cleaner for real users because the installer ISO should just show the installer when it boots, not drop to a login prompt.

   **Recommendation:** for the TEST, drive stsh through `installer`. For the PRODUCT (live ISO), add a vigil service that launches `gui-installer` on tty0 when `boot=installer` is in the kernel cmdline, and publish a new "installer ISO" make target. This is a follow-up after the GUI installer itself lands and tests green.

2. **Do we need a separate "installer USB" ISO build target?** Long-term yes (Phase 47 roadmap says "graphical version of the text-mode installer"). For this spec: no. We land the gui-installer binary and test it via stsh; the dedicated ISO target is deferred.

3. **Password masking in Glyph textfield.** `user/lib/glyph/textfield.c` doesn't currently support a mask character. The GUI installer needs it. Add a `glyph_textfield_set_mask(field, '*')` API as part of the gui-installer work, or rely on a custom draw_fn. **Recommendation:** add the API — it's 10 LOC and the feature belongs in Glyph regardless.

4. **Password confirmation error display.** When the passwords don't match, where does the error appear? Options: (a) inline label below the fields, (b) modal dialog, (c) red flash of the fields. **Recommendation:** inline label — simplest, most usable, and requires no new dialog infrastructure.

5. **Reboot button action.** On Screen 5 success, what does `[ Reboot ]` do? `sys_reboot(1)` is the kernel reboot syscall the existing menu uses. Call that directly.

Not blocking; can be settled during planning or implementation.
