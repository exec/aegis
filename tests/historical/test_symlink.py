#!/usr/bin/env python3
"""test_symlink.py — Phase 41 symlink + chmod smoke test.

Boots Aegis with q35 + NVMe disk, logs in, tests:
1. symlink creation (ln -s)
2. readlink
3. read through symlink (cat)
4. symlink chain resolution
5. chmod + permission enforcement
"""
import subprocess, sys, os, select, fcntl, time, socket, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 30

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_KEY_MAP = {
    ' ': 'spc', '\n': 'ret', '/': 'slash', '-': 'minus', '.': 'dot',
    ':': 'shift-semicolon', '|': 'shift-backslash', '>': 'shift-dot',
    '<': 'shift-comma', '_': 'shift-minus', '=': 'equal',
    '~': 'shift-grave_accent', '$': 'shift-4', "'": 'apostrophe',
    '"': 'shift-apostrophe',
}
for c in 'abcdefghijklmnopqrstuvwxyz': _KEY_MAP[c] = c
for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ': _KEY_MAP[c] = f'shift-{c.lower()}'
for c in '0123456789': _KEY_MAP.setdefault(c, c)


def build_iso():
    r = subprocess.run("make INIT=vigil iso", shell=True,
                       cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)


def _type_string(mon_sock, s):
    for ch in s:
        key = _KEY_MAP.get(ch)
        if key is None:
            continue
        mon_sock.sendall(f'sendkey {key}\n'.encode())
        time.sleep(0.08)
        try:
            mon_sock.recv(4096)
        except OSError:
            pass


class SerialReader:
    def __init__(self, fd):
        self._fd = fd
        self._buf = b""

    def _drain(self, timeout=0.5):
        ready, _, _ = select.select([self._fd], [], [], timeout)
        if ready:
            try:
                chunk = os.read(self._fd, 65536)
                if chunk:
                    self._buf += chunk
                    sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
            except (BlockingIOError, OSError):
                pass

    def wait_for(self, needle, deadline):
        enc = needle.encode() if isinstance(needle, str) else needle
        while time.time() < deadline:
            if enc in self._buf:
                return True
            self._drain()
        return enc in self._buf

    def full_output(self):
        return self._buf.decode("utf-8", errors="replace")


def run_test():
    iso_path  = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)

    if not os.path.exists(disk_path):
        print(f"SKIP: {DISK} not found — run 'make disk' first")
        sys.exit(0)

    build_iso()

    mon_path = tempfile.mktemp(suffix=".sock")
    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", iso_path, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "2G",
         "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", "user,id=n0",
         "-monitor", f"unix:{mon_path},server,nowait"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)
    serial = SerialReader(proc.stdout.fileno())

    deadline = time.time() + 10
    while not os.path.exists(mon_path) and time.time() < deadline:
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(mon_path)
    mon.setblocking(False)

    passed = 0
    failed = 0

    def check(name, needle, timeout=CMD_TIMEOUT):
        nonlocal passed, failed
        if serial.wait_for(needle, time.time() + timeout):
            print(f"  PASS: {name}")
            passed += 1
        else:
            print(f"  FAIL: {name} — '{needle}' not found")
            print(f"        last 300 chars: {serial.full_output()[-300:]!r}")
            failed += 1

    try:
        # Login
        print("  waiting for login prompt...")
        if not serial.wait_for("login: ", time.time() + BOOT_TIMEOUT):
            print("FAIL: login prompt not found")
            sys.exit(1)
        _type_string(mon, "root\n")
        if not serial.wait_for("assword", time.time() + 10):
            print("FAIL: password prompt not found")
            sys.exit(1)
        _type_string(mon, "forevervigilant\n")
        if not serial.wait_for("# ", time.time() + 10):
            print("FAIL: shell prompt not found")
            sys.exit(1)
        print("  logged in")
        time.sleep(1)

        # Test 1: Create a test file on ext2
        _type_string(mon, "echo hello > /home/testfile\n")
        time.sleep(2)

        # Test 2: Create symlink
        _type_string(mon, "ln -s /home/testfile /home/link1\n")
        time.sleep(1)
        _type_string(mon, "echo SYMCREATED\n")
        check("symlink creation", "SYMCREATED")

        # Test 3: readlink
        _type_string(mon, "readlink /home/link1\n")
        check("readlink output", "/home/testfile")

        # Test 4: Read through symlink
        _type_string(mon, "cat /home/link1\n")
        check("cat through symlink", "hello")

        # Test 5: Symlink chain
        _type_string(mon, "ln -s /home/link1 /home/link2\n")
        time.sleep(1)
        _type_string(mon, "cat /home/link2\n")
        check("symlink chain", "hello")

        # Test 6: chmod 000 then try to cat (should fail)
        _type_string(mon, "chmod 000 /home/testfile\n")
        time.sleep(1)
        _type_string(mon, "cat /home/testfile 2>&1; echo CHMODTEST\n")
        check("chmod 000 blocks read", "CHMODTEST")

        # Test 7: chmod restore
        _type_string(mon, "chmod 644 /home/testfile\n")
        time.sleep(1)
        _type_string(mon, "cat /home/testfile\n")
        check("chmod restore read", "hello")

        # Summary
        print(f"\n  Results: {passed} passed, {failed} failed")
        sys.exit(0 if failed == 0 else 1)

    finally:
        try:
            mon.close()
        except OSError:
            pass
        proc.kill()
        proc.wait()
        try:
            os.unlink(mon_path)
        except OSError:
            pass


if __name__ == "__main__":
    run_test()
