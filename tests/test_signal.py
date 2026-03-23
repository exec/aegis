#!/usr/bin/env python3
"""Phase 17 signal smoke tests. Boots the shell ISO and tests signal delivery."""
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
    """Read serial bytes from proc.stdout until '\\n# ' or '\\n#' is seen or deadline.

    The shell writes the prompt as '# ' (hash space).  In practice only the '#'
    arrives on the serial pipe before the shell blocks in fgets() waiting for
    keyboard input — the trailing space may not flush in the same QEMU serial
    buffer cycle.  Matching on '\\n#' (without requiring the trailing space)
    detects the prompt reliably without waiting for the space to arrive.

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
        if b"\n# " in buf or b"\n#" in buf:
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


def test_ctrl_c_kills_cat():
    """Run 'cat /etc/motd', send Ctrl-C, verify shell prompt returns."""
    proc, mon_sock, mon_path = _boot_qemu()

    # Wait for initial shell prompt
    _read_until_prompt(proc, time.time() + BOOT_TIMEOUT)

    # Type 'cat /etc/motd' and press Enter — cat will print motd then block on stdin.
    _type_string(mon_sock, "cat /etc/motd\n")

    # Give cat time to start and print output
    time.sleep(0.5)

    # Send Ctrl-C via QEMU monitor (SIGINT to foreground PID via kbd driver)
    _send_key(mon_sock, "ctrl-c")

    # Wait for shell prompt to reappear — shell must still be alive.
    out = _read_until_prompt(proc, time.time() + CMD_TIMEOUT)

    _teardown_qemu(proc, mon_sock, mon_path)

    # '\n# ' or '\n#' must appear in out: Ctrl-C killed cat, shell returned a new prompt.
    assert ("\n# " in out or "\n#" in out), (
        f"FAIL test_ctrl_c_kills_cat: shell prompt did not return after Ctrl-C\n"
        f"output:\n{out}"
    )
    print("PASS test_ctrl_c_kills_cat")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build_iso()
    tests = [test_ctrl_c_kills_cat]
    failures = 0
    for t in tests:
        try:
            t()
        except AssertionError as e:
            print(e)
            failures += 1
        except Exception as e:
            print(f"ERROR in {t.__name__}: {e}")
            failures += 1
    if failures:
        print(f"\n{failures}/{len(tests)} tests FAILED")
        sys.exit(1)
    print(f"\n{len(tests)}/{len(tests)} tests PASSED")
