#!/usr/bin/env bash
set -e

ISO=build/aegis.iso
EXPECTED=tests/expected/boot.txt
RAW=/tmp/aegis_serial_raw.txt
ACTUAL=/tmp/aegis_serial.txt

# ── Test 1: Boot oracle ──────────────────────────────────────────────────────
# Boot headless via GRUB-bootable ISO (INIT=vigil, -machine pc, no disk).
# Diff serial output against expected. Exit on mismatch.
timeout 60s qemu-system-x86_64 \
    -machine pc \
    -cpu Broadwell \
    -cdrom "$ISO" \
    -boot order=d \
    -display none \
    -vga std \
    -nodefaults \
    -serial stdio \
    -no-reboot \
    -m 2G \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > "$RAW" 2>/dev/null || true

sed 's/\x1b\[[?0-9;]*[A-Za-z]//g; s/\x1bc//g; s/\r//g' "$RAW" \
    | grep '^\[' \
    | grep -v '^\[DHCP\]' \
    | grep -v '^\[SHELL\]' \
    | sed 's/mounted ramdisk0, [0-9]* blocks/mounted ramdisk0, NBLK blocks/' \
    > "$ACTUAL" || true

# Apply same normalization to expected
sed 's/mounted ramdisk0, [0-9]* blocks/mounted ramdisk0, NBLK blocks/' \
    "$EXPECTED" > /tmp/aegis_expected_norm.txt

diff /tmp/aegis_expected_norm.txt "$ACTUAL"
echo "PASS boot oracle"

# ── Test 2: Hardware-specific tests (INIT=shell, separate boots) ─────────────
# These tests use INIT=shell with custom disk configs or special hardware.
# They need separate QEMU instances because each has unique device flags.

echo "--- test_nvme ---"
python3 tests/test_nvme.py

echo "--- test_ext2 ---"
python3 tests/test_ext2.py

echo "--- test_xhci ---"
python3 tests/test_xhci.py

echo "--- test_gpt ---"
python3 tests/test_gpt.py

echo "--- test_virtio_net ---"
python3 tests/test_virtio_net.py

echo "--- test_net_stack ---"
python3 tests/test_net_stack.py || exit 1

echo "--- test_login ---"
python3 tests/test_login.py

# ── Test 3: Consolidated disk tests (ONE QEMU boot) ─────────────────────────
# Boots q35+NVMe+virtio-net with INIT=vigil, logs in, runs all disk-dependent
# tests sequentially in one session. Replaces 11 separate QEMU boots.
echo "--- test_integrated (consolidated) ---"
python3 tests/test_integrated.py

# ── Test 4: Special tests (need unique setup) ───────────────────────────────
echo "--- test_socket ---"
python3 tests/test_socket.py

echo "--- test_curl ---"
python3 tests/test_curl.py

echo "--- test_installer ---"
python3 tests/test_installer.py

echo "--- test_stsh ---"
python3 tests/test_stsh.py

echo "--- test_symlink ---"
python3 tests/test_symlink.py
