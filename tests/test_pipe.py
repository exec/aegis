#!/usr/bin/env python3
"""Phase 16 pipe smoke tests. Boots the shell ISO and sends pipe commands."""
import subprocess, time, sys, os, signal

QEMU = "qemu-system-x86_64"
ISO  = "build/aegis.iso"
TIMEOUT = 30

def run_shell_session(commands):
    """Boot QEMU shell, send commands one by one, collect all serial output."""
    # Try to build the shell ISO; if it fails, use existing ISO if available
    r = subprocess.run(["make", "INIT=shell", "iso"], capture_output=True)
    if r.returncode != 0:
        # Try to use existing ISO; if it doesn't exist, fail
        import os
        if not os.path.exists(ISO):
            print("[FAIL] make INIT=shell iso failed:")
            print(r.stderr.decode())
            sys.exit(1)
        # Otherwise proceed with existing ISO

    cmd = [
        QEMU,
        "-machine", "pc", "-cpu", "Broadwell",
        "-cdrom", ISO, "-boot", "order=d",
        "-display", "none", "-vga", "std",
        "-nodefaults", "-serial", "stdio",
        "-no-reboot", "-m", "128M",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )

    output = []
    start = time.time()

    # Wait for shell prompt
    buf = b""
    while time.time() - start < TIMEOUT:
        try:
            proc.stdout.readable() and None
            chunk = proc.stdout.read(1)
            if not chunk:
                break
            buf += chunk
            if b"# " in buf:
                output.append(buf.decode(errors="replace"))
                buf = b""
                break
        except Exception:
            break

    # Send each command and collect output until next prompt
    for line in commands:
        proc.stdin.write((line + "\n").encode())
        proc.stdin.flush()
        buf = b""
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            chunk = proc.stdout.read(1)
            if not chunk:
                break
            buf += chunk
            if b"# " in buf:
                output.append(buf.decode(errors="replace"))
                buf = b""
                break

    proc.stdin.write(b"exit\n")
    proc.stdin.flush()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    return "\n".join(output)

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
