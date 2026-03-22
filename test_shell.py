#!/usr/bin/env python3
"""Drive the Aegis shell via QEMU monitor sendkey and screendump."""
import subprocess, socket, time, os, sys

ISO       = 'build/aegis.iso'
import tempfile
MON_SOCK    = tempfile.mktemp(suffix='.sock', prefix='aegis_mon_', dir='/tmp')
SERIAL_FILE = tempfile.mktemp(suffix='.txt', prefix='aegis_serial_', dir='/tmp')
SHOTS_DIR   = '/tmp/aegis_shots'

os.makedirs(SHOTS_DIR, exist_ok=True)

# MON_SOCK is freshly generated — no pre-cleanup needed

proc = subprocess.Popen([
    'qemu-system-x86_64',
    '-cdrom', ISO, '-boot', 'order=d',
    '-serial', f'file:{SERIAL_FILE}',
    '-monitor', f'unix:{MON_SOCK},server,nowait',
    '-display', 'none', '-vga', 'std',
    '-no-reboot', '-m', '128M',
    '-enable-kvm', '-cpu', 'Broadwell',
    '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# Wait for QEMU to create the socket and be ready to accept connections
mon = None
for _ in range(200):
    time.sleep(0.05)
    if not os.path.exists(MON_SOCK):
        continue
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(MON_SOCK)
        mon = s
        break
    except (ConnectionRefusedError, OSError):
        s.close()
if mon is None:
    print("ERROR: could not connect to monitor socket")
    proc.kill(); sys.exit(1)
mon.settimeout(0.3)

def mon_flush():
    try:
        while True: mon.recv(4096)
    except: pass

def mon_cmd(cmd):
    mon.sendall((cmd + '\n').encode())
    time.sleep(0.15)
    mon_flush()

time.sleep(0.2); mon_flush()

KEY_MAP = {' ': 'spc', '/': 'slash', '-': 'minus', '.': 'dot', '\n': 'ret'}

def send_key(key):
    mon_cmd(f'sendkey {key}')
    time.sleep(0.06)

def type_line(text):
    for c in text:
        if c in KEY_MAP:
            send_key(KEY_MAP[c])
        elif c.isupper():
            send_key(f'shift-{c.lower()}')
        else:
            send_key(c)
    send_key('ret')
    time.sleep(0.7)

shot_n = [0]
def screenshot(label):
    shot_n[0] += 1
    ppm = f'{SHOTS_DIR}/{shot_n[0]:02d}_{label}.ppm'
    png = ppm.replace('.ppm', '.png')
    mon_cmd(f'screendump {ppm}')
    time.sleep(0.4)
    if os.path.exists(ppm):
        subprocess.run(['pnmtopng', ppm], stdout=open(png, 'wb'), stderr=subprocess.DEVNULL)
        os.unlink(ppm)
        print(f'  screenshot → {png}')
    else:
        print(f'  screendump FAILED for {label}')

print("Waiting for boot (4 s)...")
time.sleep(4)
screenshot('boot')

commands = [
    ('ls /bin',          'ls_bin'),
    ('pwd',              'pwd'),
    ('echo hello world', 'echo'),
    ('uname',            'uname'),
    ('cat /etc/motd',    'cat_motd'),
    ('cd /bin',          'cd_bin'),
    ('pwd',              'pwd_after_cd'),
    ('help',             'help'),
    ('exit',             'exit'),
]

for cmd, label in commands:
    print(f'  -> {cmd}')
    type_line(cmd)
    screenshot(label)

time.sleep(1)
proc.terminate()
proc.wait()
mon.close()

print('\nDone. PNGs in', SHOTS_DIR)
try:
    with open(SERIAL_FILE) as f:
        lines = [l for l in f.readlines() if l.startswith('[')]
    print(f'Serial kernel lines: {len(lines)}')
    print('Last serial lines:')
    for l in lines[-5:]: print(' ', l.rstrip())
except: pass
