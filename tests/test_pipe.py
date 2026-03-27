#!/usr/bin/env python3
"""Phase 16 pipe smoke tests. Boots the shell ISO and sends pipe commands."""
import subprocess, time, sys, os, socket, select, tempfile, fcntl

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
BOOT_TIMEOUT = 900    # generous: slow/loaded host machines can take 600+ s to boot
CMD_TIMEOUT  = 120    # per-command wait after the shell prompt appears

# ---------------------------------------------------------------------------
# Key name map: ASCII char → QEMU monitor sendkey name.
# Only the characters needed for the pipe test commands are listed here.
# ---------------------------------------------------------------------------
_KEY_MAP = {
    ' ':  'spc',
    '\n': 'ret',
    '/':  'slash',
    '|':  'shift-backslash',
    '<':  'shift-comma',
    '>':  'shift-dot',
    '&':  'shift-7',
    '1':  '1',
    '2':  '2',
    '.':  'dot',
    '-':  'minus',
}
for ch in 'abcdefghijklmnopqrstuvwxyz':
    _KEY_MAP[ch] = ch
for ch in '0123456789':
    _KEY_MAP.setdefault(ch, ch)


def _char_to_key(ch):
    """Return the QEMU sendkey name for character ch, or None if unknown."""
    return _KEY_MAP.get(ch)


def build_iso():
    """Build the shell ISO once before running all tests.

    subprocess.run(['make', ...]) fails in environments where the Python process
    has euid=0 (root) but uid!=0: grub-mkrescue creates temp files owned by root,
    then mformat (running with real uid) cannot access them.  Using subprocess
    with shell=False and an explicit preexec_fn that drops back to the real uid
    works around this.
    """
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
    """Read serial bytes from proc.stdout until '\\n#' is seen or deadline.

    Uses os.read() on the raw fd with O_NONBLOCK to bypass Python's
    BufferedReader.  Python's buffered read(1) drains the entire OS pipe into
    an internal buffer on the first select() fire, leaving the fd empty so
    subsequent select() calls never trigger — the buffered bytes are then
    never consumed.  os.read() reads directly from the OS pipe, keeping
    select() and reads consistent.

    Matches '\\n#' (without requiring the trailing space) because the shell
    prompt '# ' may arrive partially: the '#' and ' ' can land in different
    QEMU serial-to-stdio write batches.
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


def run_shell_session(commands):
    """Boot QEMU shell, inject commands via PS/2 keyboard, collect serial output.

    Uses a QEMU monitor unix socket to send keyboard events so the PS/2
    keyboard driver inside the guest receives them.  Writing to QEMU's stdin
    only affects the serial console, which the shell does not read from.
    """
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
    # BOOT_TIMEOUT must exceed the actual boot time (600+ s on loaded hosts).
    # If the boot deadline fires before the shell prompt arrives, the injected
    # command races against the still-arriving boot output: _read_until_prompt
    # for the command sees '\n# ' from the first shell prompt instead of from
    # after the command output, returning too early and missing the command result.
    boot_out = _read_until_prompt(proc, time.time() + BOOT_TIMEOUT)
    all_output.append(boot_out)

    # Inject each command and collect output until next prompt
    for line in commands:
        _type_string(mon_sock, line + "\n")
        # Give the pipeline time to execute before polling
        out = _read_until_prompt(proc, time.time() + CMD_TIMEOUT)
        all_output.append(out)

    # Inject 'exit' to trigger isa-debug-exit and halt QEMU
    _type_string(mon_sock, "exit\n")
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    mon_sock.close()
    try:
        os.unlink(mon_path)
    except OSError:
        pass

    return "\n".join(all_output)


def test_pipe_ls_cat():
    out = run_shell_session(["ls /bin | cat"])
    assert "sh" in out, f"FAIL test_pipe_ls_cat: 'sh' not in output\n{out}"
    print("PASS test_pipe_ls_cat")


def test_pipe_echo_cat():
    out = run_shell_session(["echo hello | cat"])
    assert "hello" in out, f"FAIL test_pipe_echo_cat: 'hello' not in output\n{out}"
    print("PASS test_pipe_echo_cat")


def test_pipe_three_stage():
    out = run_shell_session(["ls /bin | cat | cat"])
    assert "sh" in out, f"FAIL test_pipe_three_stage: 'sh' not in output\n{out}"
    print("PASS test_pipe_three_stage")


def test_redirect_stdin():
    out = run_shell_session(["cat < /etc/motd"])
    assert "MOTD" in out or "Hello" in out, \
        f"FAIL test_redirect_stdin: motd not in output\n{out}"
    print("PASS test_redirect_stdin")


def test_stderr_redirect():
    out = run_shell_session(["ls /bin 2>&1 | cat"])
    assert "sh" in out, f"FAIL test_stderr_redirect: 'sh' not in output\n{out}"
    print("PASS test_stderr_redirect")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build_iso()
    tests = [
        test_pipe_ls_cat,
        test_pipe_echo_cat,
        test_pipe_three_stage,
        test_redirect_stdin,
        test_stderr_redirect,
    ]
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
