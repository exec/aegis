#!/usr/bin/env python3
"""Phase 18 smoke tests: stat/fstat/access/nanosleep syscalls + wc/grep/sort programs."""
import subprocess, time, sys, os, socket, select, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
BOOT_TIMEOUT = 900    # seconds; generous for loaded machines
CMD_TIMEOUT  = 120    # seconds per command after prompt appears

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
}
for ch in 'abcdefghijklmnopqrstuvwxyz':
    _KEY_MAP[ch] = ch
for ch in '0123456789':
    _KEY_MAP.setdefault(ch, ch)


def _char_to_key(ch):
    return _KEY_MAP.get(ch)


def build_iso():
    real_uid = os.getuid()
    real_gid = os.getgid()

    def drop_euid():
        os.setegid(real_gid)
        os.seteuid(real_uid)

    r = subprocess.run(["make", "INIT=shell", "iso"], preexec_fn=drop_euid)
    if r.returncode != 0:
        print("[FAIL] make INIT=shell iso failed")
        sys.exit(1)


def _send_key(mon_sock, keyname):
    cmd = ("sendkey %s\n" % keyname).encode()
    mon_sock.sendall(cmd)
    time.sleep(0.03)
    try:
        mon_sock.recv(4096)
    except BlockingIOError:
        pass


def _type_string(mon_sock, text):
    for ch in text:
        key = _char_to_key(ch)
        if key is None:
            raise ValueError("No key mapping for character %r" % ch)
        _send_key(mon_sock, key)


def _read_until_prompt(proc, deadline):
    """Read serial bytes from proc.stdout until '# ' is seen or deadline.

    Uses os.read() on the raw file descriptor to bypass Python's BufferedReader.
    Python's buffered read(1) drains the OS pipe into an internal buffer, causing
    select() to report the fd as not-ready even when bytes are buffered — the
    subsequent read(1) calls then miss those bytes.  os.read() reads directly from
    the OS pipe without buffering, keeping select() and reads consistent.
    """
    import fcntl
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


def _boot_qemu():
    """Launch QEMU and return (proc, mon_sock, mon_path). Waits for monitor socket."""
    mon_path = tempfile.mktemp(suffix=".sock", prefix="aegis_mon_")
    cmd = [
        QEMU,
        "-machine", "pc", "-cpu", "Broadwell",
        "-cdrom", ISO, "-boot", "order=d",
        "-display", "none", "-vga", "std",
        "-nodefaults", "-serial", "stdio",
        "-no-reboot", "-m", "128M",
        "-monitor", "unix:%s,server,nowait" % mon_path,
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        if os.path.exists(mon_path):
            break
        time.sleep(0.1)
    else:
        proc.kill()
        raise RuntimeError("QEMU monitor socket never appeared: %s" % mon_path)
    mon_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon_sock.connect(mon_path)
    mon_sock.setblocking(False)
    return proc, mon_sock, mon_path


def _teardown_qemu(proc, mon_sock, mon_path):
    """Inject 'exit' to halt QEMU, then clean up."""
    try:
        _type_string(mon_sock, "exit\n")
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    mon_sock.close()
    try:
        os.unlink(mon_path)
    except OSError:
        pass


def _run_cmd(proc, mon_sock, cmd_str, timeout=None):
    """Type a shell command, wait for the next prompt, return output between prompts."""
    if timeout is None:
        timeout = CMD_TIMEOUT
    _type_string(mon_sock, cmd_str + "\n")
    out = _read_until_prompt(proc, time.time() + timeout)
    return out


def test_ls_shows_new_bins():
    """ls /bin lists wc, grep, sort — verifies stat works and new binaries are present."""
    proc, mon_sock, mon_path = _boot_qemu()

    _read_until_prompt(proc, time.time() + BOOT_TIMEOUT)

    out = _run_cmd(proc, mon_sock, "ls /bin")
    _teardown_qemu(proc, mon_sock, mon_path)

    for name in ("wc", "grep", "sort"):
        assert name in out, (
            "FAIL test_ls_shows_new_bins: '%s' not found in ls /bin output\n"
            "output:\n%s" % (name, out)
        )
    print("PASS test_ls_shows_new_bins")


def test_wc_counts_bytes():
    """echo hello | wc -c prints 6 — verifies wc binary and pipe work."""
    proc, mon_sock, mon_path = _boot_qemu()

    _read_until_prompt(proc, time.time() + BOOT_TIMEOUT)

    # "echo hello\n" is 6 bytes; wc -c counts bytes
    out = _run_cmd(proc, mon_sock, "echo hello | wc -c")
    _teardown_qemu(proc, mon_sock, mon_path)

    assert "6" in out, (
        "FAIL test_wc_counts_bytes: expected '6' in output\n"
        "output:\n%s" % out
    )
    print("PASS test_wc_counts_bytes")


def test_grep_matches():
    """echo hello | grep hello prints hello — verifies grep binary works."""
    proc, mon_sock, mon_path = _boot_qemu()

    _read_until_prompt(proc, time.time() + BOOT_TIMEOUT)

    out = _run_cmd(proc, mon_sock, "echo hello | grep hello")
    _teardown_qemu(proc, mon_sock, mon_path)

    assert "hello" in out, (
        "FAIL test_grep_matches: expected 'hello' in output\n"
        "output:\n%s" % out
    )
    print("PASS test_grep_matches")


def test_cat_motd_stat():
    """cat /etc/motd prints the MOTD — verifies fstat on initrd files works."""
    proc, mon_sock, mon_path = _boot_qemu()

    _read_until_prompt(proc, time.time() + BOOT_TIMEOUT)

    out = _run_cmd(proc, mon_sock, "cat /etc/motd")
    _teardown_qemu(proc, mon_sock, mon_path)

    # /etc/motd contains "[MOTD] Hello from initrd!"
    assert "[MOTD]" in out, (
        "FAIL test_cat_motd_stat: expected '[MOTD]' in cat /etc/motd output\n"
        "output:\n%s" % out
    )
    print("PASS test_cat_motd_stat")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build_iso()
    tests = [
        test_ls_shows_new_bins,
        test_wc_counts_bytes,
        test_grep_matches,
        test_cat_motd_stat,
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
