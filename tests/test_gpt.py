#!/usr/bin/env python3
"""Phase 23 GPT partition parsing test.

Boots the shell ISO with a GPT-partitioned NVMe disk image and verifies:
  1. [GPT] OK: 2 partition(s) found on nvme0 in serial output
  2. [EXT2] OK: line contains "nvme0p1" (partition routing, not whole disk)
  3. Shell prompt appears (ext2 mount on nvme0p1 succeeded)
  4. ls /bin lists expected binaries
  5. cat /etc/motd shows "Welcome to Aegis"
"""
import subprocess, time, sys, os, socket, select, fcntl, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 900
CMD_TIMEOUT  = 120

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
    real_uid = os.getuid()
    real_gid = os.getgid()
    def drop_euid():
        os.setegid(real_gid)
        os.seteuid(real_uid)
    r = subprocess.run(["make", "INIT=shell", "iso"], preexec_fn=drop_euid)
    if r.returncode != 0:
        print("[FAIL] make INIT=shell iso failed")
        sys.exit(1)


def build_disk():
    """Rebuild disk image — GPT layout required for this test."""
    r = subprocess.run(["make", "disk"])
    if r.returncode != 0:
        print("[FAIL] make disk failed")
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


def _read_until(proc, deadline, sentinel):
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
        if sentinel.encode() in buf:
            return buf.decode(errors="replace")
    return buf.decode(errors="replace")


def run_session(commands):
    """Boot q35 + NVMe, wait for prompt, inject commands, return serial output."""
    mon_path = tempfile.mktemp(suffix=".sock", prefix="aegis_gpt_")
    cmd = [
        QEMU,
        "-machine", "q35", "-cpu", "Broadwell",
        "-cdrom", ISO, "-boot", "order=d",
        "-drive", "file=%s,format=raw,if=none,id=nvme0" % DISK,
        "-device", "nvme,drive=nvme0,serial=aegis00",
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
        raise RuntimeError("QEMU monitor socket never appeared")

    mon_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon_sock.connect(mon_path)
    mon_sock.setblocking(False)

    all_output = []
    boot_out = _read_until(proc, time.time() + BOOT_TIMEOUT, "\n#")
    all_output.append(boot_out)

    for cmd_str in commands:
        _type_string(mon_sock, cmd_str + "\n")
        time.sleep(0.1)
        out = _read_until(proc, time.time() + CMD_TIMEOUT, "\n#")
        all_output.append(out)

    proc.kill()
    proc.wait()
    mon_sock.close()
    try:
        os.unlink(mon_path)
    except FileNotFoundError:
        pass
    return "".join(all_output)


def main():
    # Normalize working directory to project root so relative paths work
    # regardless of where the script is invoked from (same pattern as test_ext2.py)
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    build_iso()
    # Always rebuild disk: this test requires GPT layout, not the old raw ext2
    if os.path.exists(DISK):
        os.unlink(DISK)
    build_disk()

    print("Running GPT partition test...")
    output = run_session(["ls /bin", "cat /etc/motd"])

    passed = True

    # 1. GPT line present
    if "[GPT] OK: 2 partition(s) found on nvme0" not in output:
        print("[FAIL] [GPT] OK line missing or wrong")
        print("Output:\n", output[-2000:])
        passed = False
    else:
        print("[PASS] [GPT] OK: 2 partition(s) found on nvme0")

    # 2. EXT2 mounts nvme0p1, not nvme0
    if "nvme0p1" not in output:
        print("[FAIL] ext2 did not mount nvme0p1")
        passed = False
    else:
        print("[PASS] ext2 mounted nvme0p1")

    # 3. Shell prompt appeared
    if "\n#" not in output:
        print("[FAIL] shell prompt never appeared")
        passed = False
    else:
        print("[PASS] shell prompt appeared")

    # 4. ls /bin lists binaries
    for binary in ["sh", "ls", "cat", "echo"]:
        if binary not in output:
            print("[FAIL] /bin/%s missing from ls output" % binary)
            passed = False
        else:
            print("[PASS] /bin/%s present" % binary)

    # 5. /etc/motd content
    if "Welcome to Aegis" not in output:
        print("[FAIL] /etc/motd content missing")
        passed = False
    else:
        print("[PASS] /etc/motd: Welcome to Aegis")

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
