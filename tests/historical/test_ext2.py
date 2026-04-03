#!/usr/bin/env python3
"""Phase 21 ext2 persistence test.

Boots the shell ISO twice with the same NVMe disk image:
  Boot 1 — write /home/test.txt via 'echo hello > /home/test.txt', verify read-back.
  Boot 2 — read /home/test.txt, verify 'hello' survived the reboot.

Uses the QEMU monitor unix socket to inject PS/2 keyboard events (same
pattern as test_pipe.py).  NVMe requires -machine q35.
"""
import subprocess, time, sys, os, socket, select, fcntl, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis-test.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 900    # generous: slow/loaded host machines can take 600+ s
CMD_TIMEOUT  = 120    # per-command wait after shell prompt appears

# ---------------------------------------------------------------------------
# Key name map: ASCII char → QEMU monitor sendkey name.
# ---------------------------------------------------------------------------
_KEY_MAP = {
    ' ':  'spc',
    '\n': 'ret',
    '/':  'slash',
    '|':  'shift-backslash',
    '<':  'shift-comma',
    '>':  'shift-dot',
    '&':  'shift-7',
    '.':  'dot',
    '-':  'minus',
    '_':  'shift-minus',
}
for ch in 'abcdefghijklmnopqrstuvwxyz':
    _KEY_MAP[ch] = ch
for ch in '0123456789':
    _KEY_MAP.setdefault(ch, ch)


def _char_to_key(ch):
    return _KEY_MAP.get(ch)


def build_iso():
    """No-op: aegis-test.iso built by 'make test' upfront."""
    if not os.path.exists(ISO):
        print("[FAIL] %s not found — run 'make test' to build" % ISO)
        sys.exit(1)


def build_disk():
    """Create the ext2 disk image (make disk), or reuse if already present."""
    if os.path.exists(DISK):
        return
    r = subprocess.run(["make", "disk"])
    if r.returncode != 0:
        print("[FAIL] make disk failed")
        sys.exit(1)


def _send_key(mon_sock, keyname):
    """Send a single key via the QEMU monitor unix socket."""
    cmd = ("sendkey %s\n" % keyname).encode()
    mon_sock.sendall(cmd)
    # Drain any monitor echo/prompt to avoid buffer overflow
    time.sleep(0.03)
    try:
        mon_sock.recv(4096)
    except BlockingIOError:
        pass


def _type_string(mon_sock, text):
    """Inject each character of text as QEMU keyboard events."""
    for ch in text:
        key = _char_to_key(ch)
        if key is None:
            raise ValueError("No key mapping for character %r" % ch)
        _send_key(mon_sock, key)


def _read_until_prompt(proc, deadline):
    """Read serial bytes from proc.stdout until '# ' is seen or deadline.

    Uses os.read() on the raw fd with O_NONBLOCK to bypass Python's
    BufferedReader (same rationale as test_pipe.py).
    """
    fd = proc.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            break
        buf += chunk
        if b"# " in buf:
            return buf.decode(errors="replace")
    return buf.decode(errors="replace")


def run_shell_session(disk_path, commands):
    """Boot QEMU (q35 + NVMe) with shell, inject commands, collect output.

    Returns the combined serial output as a string.
    """
    mon_path = tempfile.mktemp(suffix=".sock", prefix="aegis_mon_")
    cmd = [
        QEMU,
        "-machine", "q35", "-cpu", "Broadwell",
        "-cdrom", ISO, "-boot", "order=d",
        "-drive", "file=%s,format=raw,if=none,id=nvme0,cache=none" % disk_path,
        "-device", "nvme,drive=nvme0,serial=aegis00",
        "-display", "none", "-vga", "std",
        "-nodefaults", "-serial", "stdio",
        "-no-reboot", "-m", "2G",
        "-monitor", "unix:%s,server,nowait" % mon_path,
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )

    # Wait for QEMU monitor socket to appear
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        if os.path.exists(mon_path):
            break
        time.sleep(0.1)
    else:
        proc.kill()
        raise RuntimeError("QEMU monitor socket never appeared: %s" % mon_path)

    # Connect to monitor (non-blocking recv to drain prompts without hanging)
    mon_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon_sock.connect(mon_path)
    mon_sock.setblocking(False)

    all_output = []

    # Wait for initial shell prompt '\n# '.
    boot_out = _read_until_prompt(proc, time.time() + BOOT_TIMEOUT)
    all_output.append(boot_out)

    # Inject each command and collect output until next prompt
    for line in commands:
        _type_string(mon_sock, line + "\n")
        out = _read_until_prompt(proc, time.time() + CMD_TIMEOUT)
        all_output.append(out)

    # Inject 'exit' to trigger isa-debug-exit and halt QEMU
    _type_string(mon_sock, "exit\n")
    # Collect post-exit output (sync messages, System halted, etc.)
    post_exit = _read_until_prompt(proc, time.time() + 15)
    all_output.append(post_exit)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()

    mon_sock.close()
    try:
        os.unlink(mon_path)
    except OSError:
        pass

    return "\n".join(all_output)


def _ext2_find_file(disk_path, filepath):
    """Return True if filepath exists in the ext2 partition on disk_path.
    Parses the partition at LBA 2048 with 1024-byte blocks."""
    PART_OFFSET = 2048 * 512    # partition start in bytes
    BLOCK_SIZE  = 1024
    INODE_SIZE  = 128

    def read_block(f, blk):
        f.seek(PART_OFFSET + blk * BLOCK_SIZE)
        return f.read(BLOCK_SIZE)

    def read_inode(f, ino):
        import struct
        # Read superblock to get bg_inode_table
        f.seek(PART_OFFSET + 1024)
        sb = f.read(1024)
        s_inodes_per_group = struct.unpack_from('<I', sb, 40)[0]
        s_first_data_block = struct.unpack_from('<I', sb, 20)[0]
        # Read BGD (block 2 for 1024-byte blocks)
        bgd_block = 2 if s_first_data_block == 1 else 1
        f.seek(PART_OFFSET + bgd_block * BLOCK_SIZE)
        bgd = f.read(32)
        bg_inode_table = struct.unpack_from('<I', bgd, 8)[0]
        group  = (ino - 1) // s_inodes_per_group
        index  = (ino - 1) % s_inodes_per_group
        byte_off = index * INODE_SIZE
        blk_off  = byte_off // BLOCK_SIZE
        in_blk   = byte_off % BLOCK_SIZE
        f.seek(PART_OFFSET + (bg_inode_table + blk_off) * BLOCK_SIZE + in_blk)
        return f.read(INODE_SIZE)

    def scan_dir(f, dir_ino, name):
        import struct
        raw = read_inode(f, dir_ino)
        i_block_0 = struct.unpack_from('<I', raw, 40)[0]  # i_block[0]
        if i_block_0 == 0:
            return 0
        data = read_block(f, i_block_0)
        pos = 0
        while pos < BLOCK_SIZE:
            inode_val = struct.unpack_from('<I', data, pos)[0]
            rec_len   = struct.unpack_from('<H', data, pos + 4)[0]
            name_len  = data[pos + 6]
            if rec_len < 8:
                break
            if inode_val != 0:
                ent_name = data[pos + 8: pos + 8 + name_len].decode('ascii', errors='replace')
                if ent_name == name:
                    return inode_val
            pos += rec_len
        return 0

    components = [c for c in filepath.split('/') if c]
    try:
        with open(disk_path, 'rb') as f:
            cur = 2  # root inode
            for comp in components:
                cur = scan_dir(f, cur, comp)
                if cur == 0:
                    return False
        return True
    except Exception as e:
        print("  [disk-check] error: %s" % e)
        return False


def test_ext2_persistence():
    """Write a file in boot 1; verify it persists in boot 2."""
    # Use a fresh copy of the disk for each test run so tests are independent.
    fd, disk_path = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        # Copy the base disk image (built by make disk) to a temp file.
        with open(DISK, "rb") as src, open(disk_path, "wb") as dst:
            while True:
                block = src.read(1 << 20)   # 1MB chunks
                if not block:
                    break
                dst.write(block)

        # ── Boot 1: write /home/test.txt ────────────────────────────────
        print("  boot 1: writing /home/test.txt ...")
        out1 = run_shell_session(disk_path, [
            "echo hello > /home/test.txt",
            "cat /home/test.txt",
        ])

        assert "hello" in out1, (
            "FAIL test_ext2_persistence: 'hello' not in boot-1 output\n%s" % out1)
        print("  boot 1: 'hello' confirmed in cat output")
        print("  [boot1 full output follows]")
        print(out1)
        print("  [boot1 output end]")

        # Check the disk image directly before boot 2
        on_disk = _ext2_find_file(disk_path, "/home/test.txt")
        print("  disk-check after boot 1: /home/test.txt on disk = %s" % on_disk)

        # ── Boot 2: verify /home/test.txt persists ─────────────────────
        print("  boot 2: reading /home/test.txt ...")
        out2 = run_shell_session(disk_path, [
            "cat /home/test.txt",
        ])

        assert "hello" in out2, (
            "FAIL test_ext2_persistence: 'hello' not in boot-2 output "
            "(ext2 write or sync failed)\n%s" % out2)
        print("  boot 2: 'hello' persists across reboot — ext2 write confirmed")

    finally:
        os.unlink(disk_path)

    print("PASS test_ext2_persistence")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build_iso()
    build_disk()
    tests = [
        test_ext2_persistence,
    ]
    failures = 0
    for t in tests:
        try:
            t()
        except AssertionError as e:
            print(e)
            failures += 1
        except Exception as e:
            print("ERROR in %s: %s" % (t.__name__, e))
            failures += 1
    if failures:
        print("\n%d/%d tests FAILED" % (failures, len(tests)))
        sys.exit(1)
    print("\n%d/%d tests PASSED" % (len(tests), len(tests)))
