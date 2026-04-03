#!/usr/bin/env python3
"""test_socket_diag.py — Diagnostic: are Aegis sockets actually working?
Tests TCP from inside the guest AND via SLIRP port forward."""

import subprocess, time, sys, os, select, fcntl, socket, tempfile

QEMU = "qemu-system-x86_64"
ISO  = "build/aegis.iso"
DISK = "build/disk.img"
HOST_PORT = 18081

class Serial:
    def __init__(self, proc):
        self.proc = proc
        self.buf = ""

    def read_more(self, timeout=0.5):
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready, _, _ = select.select([self.proc.stdout], [], [], 0.1)
            if ready:
                try:
                    chunk = self.proc.stdout.read(4096)
                    if chunk:
                        text = chunk.decode("utf-8", errors="replace")
                        self.buf += text
                        sys.stdout.write(text)
                        sys.stdout.flush()
                except BlockingIOError:
                    pass

    def wait_for(self, needle, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.buf:
                return True
            self.read_more(0.5)
        return False

    def tail(self, n=300):
        return self.buf[-n:]

def type_via_monitor(mon_path, text):
    """Send keystrokes via QEMU monitor socket."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(mon_path)
    s.recv(4096)  # banner
    for ch in text:
        if ch == '\n':
            s.sendall(b"sendkey ret\n")
        elif ch == ' ':
            s.sendall(b"sendkey spc\n")
        elif ch == '/':
            s.sendall(b"sendkey slash\n")
        elif ch == '.':
            s.sendall(b"sendkey dot\n")
        elif ch == ':':
            s.sendall(b"sendkey shift-semicolon\n")
        elif ch == '&':
            s.sendall(b"sendkey shift-7\n")
        elif ch == '-':
            s.sendall(b"sendkey minus\n")
        elif ch == '_':
            s.sendall(b"sendkey shift-minus\n")
        elif ch == '|':
            s.sendall(b"sendkey shift-backslash\n")
        elif ch == '>':
            s.sendall(b"sendkey shift-dot\n")
        elif ch == '<':
            s.sendall(b"sendkey shift-comma\n")
        elif ch == '=':
            s.sendall(b"sendkey equal\n")
        elif ch.isalpha():
            s.sendall(f"sendkey {ch}\n".encode())
        elif ch.isdigit():
            s.sendall(f"sendkey {ch}\n".encode())
        else:
            s.sendall(f"sendkey {ch}\n".encode())
        s.recv(4096)
        time.sleep(0.02)
    s.close()

def run():
    if not os.path.exists(DISK):
        print(f"SKIP: {DISK} not found")
        sys.exit(0)

    mon_path = tempfile.mktemp(suffix=".sock")

    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", ISO, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "2G",
         "-monitor", f"unix:{mon_path},server,nowait",
         "-drive", f"file={DISK},if=none,id=nvme0,format=raw",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", f"user,id=n0,hostfwd=tcp::{HOST_PORT}-:80"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

    ser = Serial(proc)

    try:
        # Wait for login
        print("=== Waiting for login ===")
        if not ser.wait_for("login: ", timeout=120):
            print("FAIL: no login prompt")
            print(f"  tail: {ser.tail()!r}")
            return

        # Login
        time.sleep(0.5)
        type_via_monitor(mon_path, "root\n")
        if not ser.wait_for("assword", timeout=10):
            print("FAIL: no password prompt")
            return
        time.sleep(0.3)
        type_via_monitor(mon_path, "forevervigilant\n")
        if not ser.wait_for("# ", timeout=10):
            print("FAIL: no shell prompt after login")
            return
        print("  Logged in OK")

        # Wait for DHCP
        print("\n=== Waiting for DHCP ===")
        time.sleep(3)
        ser.read_more(5)
        if "DHCP" not in ser.buf:
            print("  DHCP not seen yet, waiting longer...")
            ser.wait_for("DHCP", timeout=30)

        # Test 1: Check httpd is running
        print("\n=== Test 1: Is httpd listening? ===")
        type_via_monitor(mon_path, "echo HTTPD_CHECK\n")
        ser.wait_for("HTTPD_CHECK", timeout=5)
        ser.read_more(1)

        # Test 2: Check IP configuration
        print("\n=== Test 2: Network state ===")
        # The kernel prints [NET] configured and [DHCP] acquired to serial
        if "10.0.2.15" in ser.buf:
            print("  IP 10.0.2.15 configured")
        else:
            print("  WARNING: IP not configured")

        # Test 3: Host-side TCP probe
        print("\n=== Test 3: Host TCP connect to SLIRP forward ===")
        for attempt in range(10):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(5)
                print(f"  Attempt {attempt+1}: connect localhost:{HOST_PORT}...")
                s.connect(("localhost", HOST_PORT))
                print(f"  CONNECTED! Sending GET...")
                s.sendall(b"GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
                s.settimeout(10)
                resp = b""
                while True:
                    try:
                        chunk = s.recv(4096)
                        if not chunk:
                            break
                        resp += chunk
                    except socket.timeout:
                        break
                s.close()
                print(f"  Response ({len(resp)} bytes): {resp[:300]}")
                if resp:
                    print("  PASS: got data back")
                    return
                else:
                    print("  Got connection but no data")
            except socket.timeout:
                print(f"  Attempt {attempt+1}: timeout (SYN not ACK'd)")
            except ConnectionRefusedError:
                print(f"  Attempt {attempt+1}: refused (SLIRP RST)")
            except ConnectionResetError:
                print(f"  Attempt {attempt+1}: reset")
            except Exception as e:
                print(f"  Attempt {attempt+1}: {type(e).__name__}: {e}")

            # Read serial for any TCP/networking messages
            ser.read_more(1)
            time.sleep(2)

        # Drain serial for any error messages
        print("\n=== Final serial output ===")
        ser.read_more(3)

        # Check for any TCP-related output
        print("\n=== TCP/NET related serial lines ===")
        for line in ser.buf.split('\n'):
            low = line.lower()
            if any(kw in low for kw in ['tcp', 'net', 'vnet', 'httpd', 'accept', 'connect', 'sock']):
                print(f"  {line.strip()}")

        print("\nFAIL: could not establish TCP connection")

    finally:
        proc.kill()
        proc.wait()
        try:
            os.unlink(mon_path)
        except:
            pass

if __name__ == "__main__":
    run()
