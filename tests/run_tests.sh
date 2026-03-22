#!/usr/bin/env bash
set -e

ISO=build/aegis.iso
EXPECTED=tests/expected/boot.txt
RAW=/tmp/aegis_serial_raw.txt
ACTUAL=/tmp/aegis_serial.txt

# Boot headless via GRUB-bootable ISO.
# QEMU 10 no longer supports direct ELF64 multiboot2 via -kernel (requires
# PVH ELF Note); GRUB reliably loads the kernel via the multiboot2 protocol.
#
# Exit codes:
#   QEMU exits with code 3 when kernel writes 0x01 to port 0xf4
#   (isa-debug-exit: exit_code = (value << 1) | 1 = 3).
#   timeout 10s prevents make test from hanging if the kernel stalls.
#   || true: QEMU exit code 3 triggers set -e; we ignore it here.
#
# Display flags:
#   -display none: no window (headless), but VGA device is still present.
#   -vga std: give GRUB a VGA console so grub.cfg "terminal_output console"
#     redirects GRUB's output to VGA (not serial). Without a VGA device,
#     GRUB falls back to serial, contaminating COM1 with ANSI sequences.
#   -nodefaults: suppress all other default devices.
#   -serial stdio: capture COM1 (kernel serial output) on stdout.
timeout 10s qemu-system-x86_64 \
    -machine pc \
    -cpu Broadwell \
    -cdrom "$ISO" \
    -boot order=d \
    -display none \
    -vga std \
    -nodefaults \
    -serial stdio \
    -no-reboot \
    -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > "$RAW" 2>/dev/null || true

# SeaBIOS and GRUB both write ANSI/VT100 escape sequences to COM1 before the
# kernel starts. Strip those sequences, then keep only lines that begin with
# '[' — every kernel status line follows the [SUBSYSTEM] format and nothing
# from SeaBIOS or GRUB does after ANSI stripping.
#
#   \x1b\[[?0-9;]*[A-Za-z]  — strip ESC [ ... letter  (CSI sequences)
#   \x1bc                   — strip ESC c              (terminal reset)
sed 's/\x1b\[[?0-9;]*[A-Za-z]//g; s/\x1bc//g' "$RAW" \
    | grep '^\[' \
    > "$ACTUAL" || true
# || true: grep exits 1 if no lines match (empty kernel output)

diff "$EXPECTED" "$ACTUAL"
# diff exits 0 on match, 1 on mismatch. Expected first: missing lines show as -,
# unexpected lines as +.
#
# CONSTRAINT: every kernel status line MUST begin with '[' (e.g. [SERIAL], [VGA]).
# Lines not starting with '[' are silently dropped by the grep above.
# When adding a new subsystem, verify its OK/FAIL line starts with '['.

# Phase 16 pipe smoke tests — boots the shell ISO and sends pipe commands via
# PS/2 keyboard injection.  Each test spawns its own QEMU instance.
# BOOT_TIMEOUT=900 (15 min) to handle loaded host machines; set -e is in
# effect so any Python exit code != 0 will abort this script.
python3 tests/test_pipe.py
