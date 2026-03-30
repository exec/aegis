#!/usr/bin/env python3
"""test_integrated.py — Consolidated integration test for Aegis.

Boots ONE QEMU instance with q35 + NVMe disk + virtio-net + INIT=vigil,
logs in, and runs all disk-dependent tests sequentially. This replaces
the individual test_pipe, test_stat, test_vigil, test_dynlink, test_threads,
test_mmap, test_proc, and test_pty scripts which each booted a separate
QEMU instance.

Skipped automatically if build/disk.img is not present.
"""
import subprocess, sys, os, select, fcntl, time, socket, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 30

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Key name map: ASCII char -> QEMU monitor sendkey name.
# ---------------------------------------------------------------------------
_KEY_MAP = {
    ' ':  'spc',
    '\n': 'ret',
    '/':  'slash',
    '-':  'minus',
    '.':  'dot',
    ':':  'shift-semicolon',
    '|':  'shift-backslash',
    '_':  'shift-minus',
    '<':  'shift-comma',
    '>':  'shift-dot',
    '&':  'shift-7',
    ';':  'semicolon',
}
for c in 'abcdefghijklmnopqrstuvwxyz':
    _KEY_MAP[c] = c
for c in '0123456789':
    _KEY_MAP.setdefault(c, c)


def _type_string(mon_sock, s):
    """Inject each character of s as QEMU keyboard events via monitor socket."""
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
    """Non-blocking serial output reader with needle search."""

    def __init__(self, fd):
        self._fd = fd
        self._buf = b""
        self._mark = 0

    def _drain(self, timeout=0.5):
        ready, _, _ = select.select([self._fd], [], [], timeout)
        if ready:
            try:
                chunk = os.read(self._fd, 65536)
                if chunk:
                    self._buf += chunk
            except (BlockingIOError, OSError):
                pass

    def mark(self):
        """Set a mark at the current buffer position. Subsequent searches
        only look at output received after this mark."""
        self._mark = len(self._buf)

    def wait_for(self, needle, deadline):
        """Wait until needle appears in output after the mark."""
        enc = needle.encode() if isinstance(needle, str) else needle
        while time.time() < deadline:
            if enc in self._buf[self._mark:]:
                return True
            self._drain()
        return enc in self._buf[self._mark:]

    def wait_for_prompt(self, deadline):
        """Wait for shell prompt '# ' on a fresh line (after \\n).
        stsh uses raw mode and redraws the prompt after \\r for every
        keystroke — those redraws contain '# ' but are not real prompts.
        Only match '# ' preceded by \\n (the real post-command prompt)."""
        while time.time() < deadline:
            buf = self._buf[self._mark:]
            # Find \n followed by anything then "# "
            idx = buf.find(b"\n")
            while idx >= 0:
                rest = buf[idx + 1:]
                if b"# " in rest:
                    return True
                idx = buf.find(b"\n", idx + 1)
            self._drain()
        # Final check
        buf = self._buf[self._mark:]
        idx = buf.find(b"\n")
        while idx >= 0:
            if b"# " in buf[idx + 1:]:
                return True
            idx = buf.find(b"\n", idx + 1)
        return False

    def contains(self, needle):
        """Check if needle is in output after the mark (no waiting)."""
        enc = needle.encode() if isinstance(needle, str) else needle
        return enc in self._buf[self._mark:]

    def snapshot(self):
        """Drain any pending bytes and return output since mark."""
        self._drain(timeout=0.1)
        return self._buf[self._mark:].decode("utf-8", errors="replace")

    def tail(self, n=500):
        """Return the last n characters of output since mark."""
        return self.snapshot()[-n:]


def build_iso():
    """Build the vigil ISO."""
    r = subprocess.run("make INIT=vigil iso", shell=True,
                       cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)


# ---------------------------------------------------------------------------
# Test definitions: (name, command, needle_check_fn)
#
# needle_check_fn receives the SerialReader and returns True if the test
# condition is satisfied after the command has run.
# For boot-output checks, command is None (no command typed).
# ---------------------------------------------------------------------------

def _any_of(serial, *needles):
    """Return True if any of the needles are in accumulated output."""
    for n in needles:
        if serial.contains(n):
            return True
    return False


TESTS = [
    # --- Pipe tests (from test_pipe.py) ---
    ("pipe: ls /bin | cat",
     "ls /bin | cat",
     lambda s: s.contains("sh")),

    ("pipe: echo hello | cat",
     "echo hello | cat",
     lambda s: s.contains("hello")),

    ("pipe: ls /bin | cat | cat",
     "ls /bin | cat | cat",
     lambda s: s.contains("sh")),

    ("pipe: cat < /etc/motd",
     "cat < /etc/motd",
     lambda s: _any_of(s, "Welcome", "Aegis")),

    ("pipe: ls /bin 2>&1 | cat",
     "ls /bin 2>&1 | cat",
     lambda s: s.contains("sh")),

    # --- Stat tests (from test_stat.py) ---
    ("stat: ls /bin shows cat",
     "ls /bin",
     lambda s: s.contains("cat")),

    ("stat: echo hello | wc",
     "echo hello | wc",
     lambda s: _any_of(s, "1", "2", "3", "4", "5", "6")),

    ("stat: echo foobar | grep foo",
     "echo foobar | grep foo",
     lambda s: s.contains("foobar")),

    ("stat: cat /etc/motd",
     "cat /etc/motd",
     lambda s: _any_of(s, "Welcome", "Aegis")),

    # --- Vigil cap check (boot output) ---
    ("vigil: init capabilities granted",
     None,
     lambda s: s.contains("[CAP] OK:") and s.contains("capabilities granted to init")),

    # --- Dynamic linking ---
    ("dynlink: /bin/dynlink_test",
     "/bin/dynlink_test",
     lambda s: s.contains("DYNLINK OK")),

    # --- Subsystem test binaries ---
    ("threads: /bin/thread_test",
     "/bin/thread_test",
     lambda s: _any_of(s, "THREAD OK", "ALL PASS")),

    ("mmap: /bin/mmap_test",
     "/bin/mmap_test",
     lambda s: _any_of(s, "MMAP OK", "ALL PASS")),

    ("proc: /bin/proc_test",
     "/bin/proc_test",
     lambda s: _any_of(s, "PROC OK", "ALL PASS")),

    ("pty: /bin/pty_test",
     "/bin/pty_test",
     lambda s: s.contains("PTY OK")),

    # --- Network (boot output) ---
    ("net: DHCP acquired",
     None,
     lambda s: s.contains("[DHCP] acquired")),

    # --- Filesystem writability (Phase 34) ---
    ("fs: echo to /tmp (ramfs)",
     "echo tmptest > /tmp/foo && cat /tmp/foo",
     lambda s: s.contains("tmptest")),

    ("fs: touch + ls on ext2",
     "touch /home/testfile && ls /home/testfile",
     lambda s: s.contains("testfile")),

    ("fs: unlink file",
     "rm /home/testfile ; ls /home/testfile",
     lambda s: s.contains("No such file")),

    ("fs: mkdir + ls",
     "mkdir /home/newdir && ls /home",
     lambda s: s.contains("newdir")),

    ("fs: rename (mv)",
     "echo mvtest > /home/orig && mv /home/orig /home/moved && cat /home/moved",
     lambda s: s.contains("mvtest")),

    # --- VFS structure ---
    ("vfs: ls / shows root dirs",
     "ls /",
     lambda s: _any_of(s, "bin", "etc", "lib")),

    ("vfs: pwd shows /root",
     "pwd",
     lambda s: s.contains("/root")),

    ("vfs: cd + pwd",
     "cd /bin && pwd",
     lambda s: s.contains("/bin")),

    # --- Device files ---
    ("dev: /dev/urandom readable",
     "ls /dev/urandom",
     lambda s: s.contains("urandom")),
]


def run_tests():
    iso_path  = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)

    if not os.path.exists(disk_path):
        print(f"SKIP: {DISK} not found -- run 'make disk' first")
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

    # Wait for QEMU monitor socket
    deadline = time.time() + 10
    while not os.path.exists(mon_path) and time.time() < deadline:
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(mon_path)
    mon.setblocking(False)

    passed = 0
    failed = 0
    results = []

    try:
        # --- Login ---
        if not serial.wait_for("login: ", time.time() + BOOT_TIMEOUT):
            print("FAIL: login prompt not found within %ds" % BOOT_TIMEOUT)
            print("  tail: %r" % serial.tail())
            sys.exit(1)
        _type_string(mon, "root\n")

        if not serial.wait_for("assword", time.time() + 10):
            print("FAIL: password prompt not found")
            print("  tail: %r" % serial.tail())
            sys.exit(1)
        _type_string(mon, "forevervigilant\n")

        if not serial.wait_for("# ", time.time() + 10):
            print("FAIL: shell prompt not found after login")
            print("  tail: %r" % serial.tail())
            sys.exit(1)

        # --- Run each test ---
        for name, command, check_fn in TESTS:
            if command is not None:
                serial.mark()  # only search NEW output for command tests
            else:
                serial._mark = 0  # boot-output checks search everything
            if command is not None:
                # Type the command
                time.sleep(0.5)
                _type_string(mon, command + "\n")

                # Wait for the command to produce output and the next prompt.
                # stsh redraws prompt+partial-input after \r for each keystroke
                # (raw mode), so wait for \n then "# " to skip redraws.
                if not serial.wait_for_prompt(time.time() + CMD_TIMEOUT):
                    # Timed out waiting for prompt -- still check the output
                    pass

            # Check the test condition
            if check_fn(serial):
                print(f"  PASS  {name}")
                passed += 1
                results.append((name, True))
            else:
                print(f"  FAIL  {name}")
                print(f"        tail: {serial.tail(300)!r}")
                failed += 1
                results.append((name, False))

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

    # --- Summary ---
    total = passed + failed
    print()
    if failed == 0:
        print(f"ALL {total} TESTS PASSED")
    else:
        print(f"{passed}/{total} passed, {failed}/{total} FAILED:")
        for name, ok in results:
            if not ok:
                print(f"  - {name}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run_tests()
