# Booting Aegis on Raspberry Pi 5

Step-by-step guide to flashing Aegis onto a Pi 5 and getting it booted over a serial console. This is the practical "I have the hardware in front of me" companion to [ARM64.md](ARM64.md) §17-18, which has the technical deep dive on GIC-400, DTB parsing, and memory layout.

**Hardware status as of 2026-04-12:** untested on real silicon. Everything in this guide has been verified under `qemu-system-aarch64 -machine virt -cpu cortex-a72` which approximates BCM2712 closely but not perfectly. First-boot diagnosis on real hardware requires a serial cable plugged into the Pi 5's dedicated JST-SH debug header.

---

## 1. Shopping list

You need three things besides the Pi 5:

| Item | Cost | Notes |
|------|------|-------|
| **Raspberry Pi Debug Probe** | $12 | First-party JST-SH debug cable. See §2 for why this and not a 40-pin GPIO cable. |
| **SD card, any size ≥ 128 MB, FAT32-formatted** | ~$5 | The Aegis boot image is ~2 MB. Any microSD card works. |
| **USB-C power supply for the Pi 5** | ~$15 | 27W official Pi 5 PSU recommended. 5V 3A minimum. |

You do **not** need: HDMI cable, monitor, USB keyboard, Ethernet cable, Wi-Fi. All interaction with Aegis during first boot is through the serial console on the JST-SH debug header.

---

## 2. The serial cable

### Why the 40-pin GPIO header does not work on Pi 5

On Pi 4 and earlier, the debug UART (UART0) was wired directly from the BCM283x/BCM271x SoC to pins 8 and 10 of the 40-pin GPIO header (GPIO 14/15). A $10 USB-TTL cable like the Adafruit #954 dropped onto three pins and you were done.

**This does not work on Pi 5.** The BCM2712 routes its 40-pin GPIO through the RP1 south bridge — a dedicated I/O companion chip on the PCIe x4 link. To reach GPIO 14/15 from software you'd need:

1. A PCIe host controller driver for the BCM2712 internal PCIe root complex
2. An RP1 driver that knows the (partially documented) register map
3. Configuration for RP1's own UART0 block

None of that exists in Aegis yet. Running the 40-pin cable will produce silence.

What Aegis *does* use on Pi 5 is the BCM2712 **internal** PL011 debug UART at physical address `0x107D001000`. This UART is not on the 40-pin header at all — it's on a dedicated 3-pin JST-SH connector silkscreened "UART" between the two micro-HDMI ports on the board edge. The on-board SPI EEPROM bootloader pre-initializes it to 115200 8N1 when `enable_uart=1` is set in `config.txt`.

### Primary recommendation: Raspberry Pi Debug Probe

**[raspberrypi.com/products/debug-probe](https://www.raspberrypi.com/products/debug-probe/)** — $12, first-party, designed for exactly this purpose. One end is a JST-SH 3-pin plug that snaps directly onto the Pi 5's debug header. The other end is USB-A. On the laptop it shows up as a standard USB CDC serial device (`/dev/tty.usbmodem*` on macOS, `/dev/ttyACM0` on Linux). No drivers, no wiring diagram, no wire-color mnemonic to memorize.

It also exposes an SWD probe on a second cable for flashing firmware, which we don't need — just the UART side.

### Alternatives

- **Adafruit #5054** — a bare JST-SH 3-pin cable (~$2). Plug one end into the Pi 5 and wire the other three leads to any USB-TTL adapter you already own (CP2102, FT232RNL, etc). You'll need to match pinout by hand: the JST-SH pinout is documented on the [Pi 5 hardware page](https://www.raspberrypi.com/documentation/computers/raspberry-pi-5.html).
- **Generic JST-SH-to-Dupont jumper cables** (~$5-10 on Amazon) — same idea, pre-terminated as female Dupont jumpers on the non-Pi end so you can breadboard them into an existing USB-TTL adapter.

### What NOT to buy

- **Adafruit #954** — the classic CP2102 cable with 2.54 mm Dupont female headers. Works beautifully on Pi 4. Will not work on Pi 5 without also acquiring a JST-SH adapter *and* an RP1 driver. Skip it.
- **Any "Pi GPIO UART" cable that terminates in a 4-pin Dupont block** — those target GPIO 14/15, which is RP1-only on Pi 5.
- **RS-232 serial adapters (DB9 connectors)** — ±12 V logic, would destroy the Pi. You want **TTL** serial, 3.3 V logic.

### On running the Debug Probe *and* an HDMI monitor

You can still plug HDMI displays into the two micro-HDMI ports while the JST-SH cable is connected; the UART header sits between them and the plug is low-profile. No physical conflict.

---

## 3. The Pi 5 debug header

Find the two micro-HDMI ports on the edge of the Pi 5. Between them, closer to the USB-C power port, is a small white 3-pin horizontal connector silkscreened **"UART"**. That is the JST-SH 1.0 mm 3-pin debug header.

```
     board edge, USB-C on the left
     ─────────────────────────────────────────
       USB-C ┃  ┃ HDMI0 ┃  ┃ UART ┃  ┃ HDMI1 ┃
      power  ┃  ┃ micro ┃  ┃ JST  ┃  ┃ micro ┃
             ┗━━┛       ┗━━┛ SH   ┗━━┛       ┗━━
                              ▲
                              │
                              └── plug the Debug Probe here
```

The three pins on the header are, reading from the inside of the board outward: **TX**, **RX**, **GND**. The Raspberry Pi Debug Probe cable is keyed so it only inserts one way; you cannot get the polarity wrong with a first-party cable.

If you're wiring a bare JST-SH adapter to a generic USB-TTL module by hand, the rules are the same as any serial link:

- Pi **TX** → adapter **RX** (data flowing out of the Pi into the laptop)
- Pi **RX** → adapter **TX** (data flowing from the laptop into the Pi)
- Pi **GND** → adapter **GND**
- Do **not** connect any VCC/5V/3.3V line — the Pi is self-powered from its USB-C port

The two most common mistakes with hand-wired adapters are swapping TX/RX (you'll see silence) and accidentally connecting a second power rail (you'll see boot timing jitter or back-powering).

With the first-party Debug Probe: none of the above is your problem. Plug it in, done.

---

## 4. Building the Pi 5 image

From the project root on branch `arm64-port`:

```bash
# 1. Build the ARM64 kernel (Linux arm64 Image format)
make -C kernel/arch/arm64 image

# 2. Package it with Pi firmware into a ready-to-flash directory
bash tools/build-pi5-image.sh
```

The first run downloads the Pi 5 device tree blob (~80 KB) and the optional BCM2712 D0-stepping overlay (~1 KB) from [github.com/raspberrypi/firmware](https://github.com/raspberrypi/firmware/tree/master/boot) and caches them in `references/pi-firmware/`. Subsequent runs are offline.

**Output:** `build/pi5-image/` containing:

```
bcm2712-rpi-5-b.dtb    78 KB    # Pi 5 device tree (firmware auto-selects this name)
overlays/
  bcm2712d0.dtbo        1 KB    # optional: D0-stepping overlay for rev 1.1 boards
config.txt              2 KB    # boot config
kernel_2712.img       ~900 KB   # Aegis kernel in Linux arm64 Image format
```

Total: ~1 MB. Significantly smaller than a Pi 4 image because Pi 5 does not use `bootcode.bin`, `start4.elf`, `fixup4.dat`, or any other pre-kernel firmware files from the SD card — that second-stage bootloader lives entirely in the on-board SPI EEPROM.

The boot flow on Pi 5 is:

```
BCM2712 boot ROM          (immutable, on-die)
      │
      ▼
SPI EEPROM bootloader     (on-board flash — NOT on the SD card)
      │
      ▼
TF-A BL31 at EL3          (built into the EEPROM)
      │
      ▼
Aegis kernel at EL2       (loaded from the SD card as kernel_2712.img)
```

The SD card only needs three files (plus the optional overlay subdirectory).

### About `bcm2712d0.dtbo`

Pi 5 boards exist in two silicon revisions. The original release used BCM2712 C-stepping. Newer 2 GB and 16 GB SKUs (board revision 1.1) use BCM2712 D-stepping, which has small register-layout quirks that need an overlay to boot cleanly. The [Circle](https://github.com/rsta2/circle) bare-metal library documents this as a hard requirement for rev 1.1 boards.

The packager fetches the overlay best-effort and drops it under `overlays/`. On C-stepping silicon it's harmless and ignored. On D-stepping silicon the EEPROM bootloader applies it automatically based on the detected board revision. Including it costs a kilobyte and eliminates a class of "why doesn't my Pi 5 boot" complaints.

---

## 5. config.txt contents

The build script writes this exact file to the root of the image:

```
kernel=kernel_2712.img
kernel_address=0x40200000
enable_uart=1
os_check=0
pciex4_reset=0
```

Per-line rationale:

- **`kernel=kernel_2712.img`** — canonical Pi 5 kernel filename. The Pi 5 EEPROM bootloader looks for `kernel_2712.img` by default; `kernel8.img` still works as a backward-compatible fallback, but the new name is more honest about what it is.
- **`kernel_address=0x40200000`** — where the firmware loads the kernel image in physical RAM. Our linker pins `KERN_LMA = 0x40200000`, so we load there directly. Pi 5 RAM starts at physical `0x00000000` (unlike Pi 4, where it started at `0x40000000`), so `0x40200000` is always valid RAM on any Pi 5 SKU (4 GB / 8 GB / 16 GB all cover it).
- **`enable_uart=1`** — enables and pre-initializes the BCM2712 internal PL011 debug UART at `0x107D001000` for 115200 8N1 from a 48 MHz reference clock. This is the UART exposed on the JST-SH header. It is *not* the same as GPIO 14/15.
- **`os_check=0`** — suppresses the firmware's "This OS does not indicate support for Raspberry Pi 5" banner that would otherwise be printed to our debug UART before Aegis even runs. Cosmetic but noisy.
- **`pciex4_reset=0`** — do NOT reset the PCIe x4 root complex at OS handoff. RP1 hangs off this link, and resetting it leaves RP1 in an indeterminate state. Leaving it alone keeps RP1 in whatever configuration the EEPROM established, so a future RP1 driver has a clean starting point.

Things that were in the old Pi 4 config but are **deliberately absent**:

- `arm_64bit=1` — ignored on Pi 5 (Cortex-A76 is AArch64-only)
- `device_tree=...` — Pi 5 firmware auto-selects `bcm2712-rpi-5-b.dtb` and applies D-stepping overlays automatically; explicitly setting this breaks overlay selection
- `uart_2ndstage=1` — that flag controls the *RP1* UART0 path (for GPIO 14/15 users), which we don't have a driver for anyway
- `disable_commandline_tags=1` — we don't parse firmware cmdline args; default is fine

---

## 6. Flashing the SD card

### macOS

```bash
# Identify the SD card device (will be /dev/diskN where N is the number)
diskutil list

# Format as FAT32, single partition. Replace 'diskN' with your actual device.
diskutil eraseDisk FAT32 AEGISBOOT MBRFormat /dev/diskN

# Copy everything from build/pi5-image/ to the card root (note -r for overlays/)
cp -rv build/pi5-image/* /Volumes/AEGISBOOT/

# Flush and eject
diskutil eject /dev/diskN
```

**Warning:** `diskutil eraseDisk` will destroy everything on whatever disk you point it at. Double-check `diskN` is your SD card and not your internal drive. Your Mac's internal SSD is almost always `disk0` or `disk1`, and SD cards are usually `disk3` or higher.

### Linux

```bash
# Identify the SD card
lsblk

# Partition and format (replace /dev/sdX with your actual device)
sudo parted /dev/sdX --script mklabel msdos mkpart primary fat32 1MiB 100%
sudo mkfs.vfat -F 32 -n AEGISBOOT /dev/sdX1

# Mount and copy
sudo mkdir -p /mnt/sd
sudo mount /dev/sdX1 /mnt/sd
sudo cp -rv build/pi5-image/* /mnt/sd/
sudo umount /mnt/sd
```

Same warning: verify `/dev/sdX` before running. `sudo fdisk -l` will show sizes so you can tell your SSD apart from the SD card.

After flashing, the SD card root should contain exactly:

```
/bcm2712-rpi-5-b.dtb
/config.txt
/kernel_2712.img
/overlays/bcm2712d0.dtbo   (optional, present if the fetch succeeded)
```

Nothing else. No `bootcode.bin`, no `start*.elf`, no `fixup*.dat`. If those files appear on your card from an earlier Pi 4 experiment, they're harmless on Pi 5 but can be deleted.

---

## 7. Physical setup

1. **Insert the SD card** into the Pi 5's slot (underside of the board, near the USB-C port).
2. **Plug the Debug Probe** into the JST-SH "UART" header between the two micro-HDMI ports. The connector is keyed — only goes in one way.
3. **Plug the Debug Probe's USB end** into your laptop. Don't power the Pi yet.
4. **Open a serial terminal** on your laptop (see §8). Leave it running, ready to capture output.
5. **Plug the Pi 5 into USB-C power.** Boot starts immediately.

---

## 8. Serial terminal on your laptop

### macOS

```bash
# Find the serial device (cable must be plugged in)
ls /dev/tty.usbmodem*       # Debug Probe (CDC ACM)
ls /dev/tty.usbserial-*     # USB-TTL adapter (CP2102/FT232)

# Open terminal (substitute the device you actually see)
screen /dev/tty.usbmodem14101 115200
```

Or if you prefer `minicom`:

```bash
brew install minicom
minicom -b 115200 -o -D /dev/tty.usbmodem14101
```

### Linux

```bash
# Find the serial device
ls /dev/ttyACM*             # Debug Probe (CDC ACM)
ls /dev/ttyUSB*             # USB-TTL adapter

# You may need to add yourself to the dialout group first:
sudo usermod -aG dialout $USER   # then log out + back in

# Open terminal
screen /dev/ttyACM0 115200
```

Or:

```bash
sudo apt install minicom
minicom -b 115200 -o -D /dev/ttyACM0
```

### Exiting `screen`

Press **Ctrl-A**, then **K**, then **y** to confirm. If your terminal refuses to close cleanly: `killall screen` from another terminal.

### Settings

Baud rate **115200**, **8 data bits**, **no parity**, **1 stop bit** (8N1). No flow control. These are the defaults for both `screen` and `minicom` with the commands above, so you shouldn't need to change anything.

---

## 9. Expected boot output

### Success

If everything works, within a second of powering on the Pi you'll see output like:

```
[BOOT] aegis arm64 (pre-MMU)
[SERIAL] OK: PL011 UART initialized
[PMM] OK: 4096MB usable across 1 regions
[VMM] OK: ARM64 4KB-granule page tables active
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[GIC] OK: GICv2 initialized                     ← Pi 5 uses GIC-400
[TIMER] OK: ARM generic timer at 100 Hz
[CAP] OK: capability subsystem initialized
[RNG] OK: ChaCha20 CSPRNG seeded
[VFS] OK: initialized
[INITRD] OK: 33 files registered
[CAP] OK: 7 baseline capabilities granted to init
[SCHED] OK: scheduler started, 2 tasks
vigil: starting
vigil: boot mode: text
vigil: getty
...

 _______ _______  ______ _____ _______
 |_____| |______ |  ____   |   |______
 |     | |______ |_____| __|__ ______|

 WARNING: This system is restricted to authorized users.
 All activity is monitored and logged. Unauthorized access
 will be investigated and may result in prosecution.


login:
```

Note that the GIC line reads **`GICv2 initialized`**, not GICv3. BCM2712 uses ARM **GIC-400**, which is a GICv2 implementation — despite Pi 5 being a Cortex-A76-class chip. Earlier versions of this guide incorrectly claimed GICv3; that was wrong, and the runtime dispatcher in `kernel/arch/arm64/gic.c` picks GIC-400 on Pi 5 based on the compatible string in the DTB.

If you reach the `login:` prompt, **everything works**. See §12 for known userland limitations.

### Partial success: firmware talks, kernel doesn't

```
[firmware banner from EEPROM]
...
[then nothing]
```

Means the EEPROM bootloader loaded `config.txt`, loaded `kernel_2712.img`, jumped into it — but our kernel isn't printing anything. Most likely causes:

1. **PL011 MMIO probe mismatch.** `uart_pl011.c` is looking for the UART at a physical address that doesn't match `0x107D001000`. Verify via `make sym` and the DTB parser output.
2. **Stale `kernel_address`.** If the firmware loaded the kernel at `0x80000` but our linker pinned it at `0x40200000`, the Image header's self-relocation may not fix it up in time. Check the config.txt you actually flashed.
3. **MMU early bring-up hanging.** If you see `[BOOT] aegis arm64 (pre-MMU)` but nothing after, MMU enable in `boot.S` is looping. That's an Agent D area — report the last line you see.

### Complete silence

No output at all. The Pi power LED lights up (so power is fine) but nothing appears on the serial terminal.

**Most likely: debug cable issue.**

1. Is the Debug Probe (or your JST-SH adapter) actually plugged into the **UART** header between the micro-HDMI ports? Not the 40-pin GPIO header, not the debug header for SWD.
2. Is `enable_uart=1` present in `config.txt`? Without it the EEPROM bootloader leaves the PL011 clock gated and the UART produces nothing.
3. Is your terminal at 115200 baud, not 9600 / 38400 / 57600?
4. If you built your own JST-SH adapter: are TX and RX crossed over (Pi TX → adapter RX, Pi RX → adapter TX)? Straight-through wiring produces silence.

**If the cable is definitely correct:** check the Pi's ACT LED (the green one next to the power LED). On boot it should blink briefly. If it doesn't blink at all, the EEPROM is refusing to load — check that the SD card is FAT32, that `kernel_2712.img` and `config.txt` are in the root (not a subdirectory), and that the DTB file is named exactly `bcm2712-rpi-5-b.dtb`. If the ACT LED blinks a pattern (e.g., 4 long + 4 short), that's an EEPROM error code — [Pi Foundation docs](https://www.raspberrypi.com/documentation/computers/configuration.html#led-warning-flash-codes) explain the patterns.

### Kernel crashes partway through

If you see some `[XXXX] OK` lines but then a panic or hang:

- **Stops after `[SERIAL] OK`, before `[PMM] OK`:** DTB parsing is failing. The kernel can't find memory regions. Capture the exact last line and report back.
- **Stops after `[KVA] OK`, before `[GIC] OK`:** GIC-400 driver is hanging. Capture the exact last line.
- **Stops after `[GIC] OK`, before `[TIMER] OK`:** timer PPI routing isn't working. Capture the exact last line.
- **`[PANIC]` followed by `ELR=...`, `ESR=...`:** a synchronous exception. **Paste the entire panic block back** and it's directly resolvable — ELR resolves to a source line via `addr2line`.

---

## 10. Iterating on a failed boot

If the kernel fails, you don't need to rebuild the entire SD card. Only `kernel_2712.img` changes between builds. Workflow:

```bash
# 1. Edit kernel source (fix the bug)
vim kernel/arch/arm64/uart_pl011.c

# 2. Rebuild the Image
make -C kernel/arch/arm64 image

# 3. Copy just the new kernel to the SD card
cp kernel/arch/arm64/build/aegis.img /Volumes/AEGISBOOT/kernel_2712.img
# or on Linux: sudo cp build/pi5-image/kernel_2712.img /mnt/sd/kernel_2712.img

# 4. Eject, reinsert into Pi, power cycle
```

A full cycle (edit → build → copy → reboot → read serial) is 30-60 seconds once you have the rhythm.

---

## 11. What works and what doesn't on Pi 5

### Works (or expected to work, pending hardware verification)

- ARM64 kernel boot from Linux arm64 Image format
- BCM2712 internal PL011 debug UART (`0x107D001000`) for console
- ARM generic timer at 100 Hz
- GIC-400 interrupt controller (GICv2)
- Memory discovery from DTB (up to 16 GB)
- Kernel scheduler, VFS, initrd, capability subsystem (Rust)
- Vigil init system + service supervision
- Interactive login prompt

### Does not work yet (all RP1-dependent)

Pi 5 moves almost every I/O peripheral behind the RP1 south bridge on a PCIe x4 link. Until Aegis has a PCIe root complex driver and an RP1 driver, none of the following function:

- **GPIO 14/15 UART** — RP1 owns the 40-pin header. BCM2712 internal UART (the JST-SH one) works, the 40-pin UART does not.
- **USB** — USB 2.0 and USB 3.0 on Pi 5 are RP1-attached.
- **Ethernet** — the gigabit MAC is inside RP1.
- **Wi-Fi / Bluetooth** — attached via a separate PCIe link, same general story.
- **GPIO as I/O pins** — RP1 owns all 40 pins.

Non-RP1 items that also don't work yet:

- **HDMI / display** — no display driver, no DRM stack. Serial-only first boot.
- **SD card as read/write storage** — only used as boot media; we don't have an SD host controller driver.
- **Multi-core** — kernel runs single-core only on ARM64. Pi 5's 3 extra Cortex-A76 cores sit idle.
- **Interactive shell after login** — userland path under active rebuild; you'll reach `login:` but credentials may not validate yet depending on the rebuild state.

What does work is useful: you can verify the kernel runs on real silicon, measure boot performance, and test UART + GIC-400 + timer driver correctness. That's the foundation for everything else.

---

## 12. Reporting failures for remote debugging

If you can copy text out of your serial terminal: paste the entire boot trace from the first line back to me, and I can usually point at the bug in one round-trip.

Most useful details:

1. **Everything from the start of output through the last line before the hang or panic.** Don't trim — sometimes a subtle earlier line is the real clue. `[BOOT] aegis arm64 (pre-MMU)` is the earliest print point.
2. **The Pi EEPROM version** (printed by the EEPROM bootloader itself when `enable_uart=1` is set). EEPROM versions sometimes change boot behavior, especially around D-stepping overlays.
3. **Which Pi 5 model and revision** — 4 GB / 8 GB / 16 GB, and whether it's board revision 1.0 (C-stepping) or 1.1 (D-stepping). The sticker on the board or `/proc/cpuinfo` on a Linux reference image will tell you.
4. **Your debug cable** (Raspberry Pi Debug Probe, Adafruit #5054, DIY JST-SH adapter) — different cables have slightly different USB CDC descriptors and timing characteristics.

If your terminal can't copy text (e.g., you're in `screen` inside a VM and the clipboard is weird): take a phone photo of the screen. Legible is enough.

---

## 13. Reflashing from scratch

If the `build/pi5-image/` directory is missing or outdated and `tools/build-pi5-image.sh` fails:

```bash
# Nuke the cache and re-fetch
rm -rf references/pi-firmware/ build/pi5-image/
bash tools/build-pi5-image.sh
```

This re-downloads the DTB and overlay from GitHub. Requires internet.

If GitHub is blocked or you're offline, `bcm2712-rpi-5-b.dtb` and `overlays/bcm2712d0.dtbo` can be extracted from any recent Raspberry Pi OS image — mount the boot partition and copy those two files into `references/pi-firmware/` (preserving the `overlays/` subdirectory), then re-run the script.

---

## 14. Hardware references

- Raspberry Pi Debug Probe — [raspberrypi.com/products/debug-probe](https://www.raspberrypi.com/products/debug-probe/)
- Pi 5 hardware overview — [raspberrypi.com/documentation/computers/raspberry-pi-5.html](https://www.raspberrypi.com/documentation/computers/raspberry-pi-5.html)
- Pi 5 boot flow & EEPROM bootloader — [raspberrypi.com/documentation/computers/raspberry-pi-5.html](https://www.raspberrypi.com/documentation/computers/raspberry-pi-5.html)
- config.txt reference — [raspberrypi.com/documentation/computers/config_txt.html](https://www.raspberrypi.com/documentation/computers/config_txt.html)
- Pi firmware repository — [github.com/raspberrypi/firmware](https://github.com/raspberrypi/firmware)
- Circle bare-metal library (definitive Pi 5 bare-metal reference) — [github.com/rsta2/circle](https://github.com/rsta2/circle)
- BCM2712 peripherals datasheet — [datasheets.raspberrypi.com/bcm2712/bcm2712-peripherals.pdf](https://datasheets.raspberrypi.com/bcm2712/bcm2712-peripherals.pdf)
- RP1 peripherals datasheet (draft) — [datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf)
- Pi 5 DTS source — [github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts](https://github.com/raspberrypi/linux)
- ARM GICv2 architecture spec (applies to GIC-400) — [developer.arm.com/documentation/ihi0048](https://developer.arm.com/documentation/ihi0048/latest)
- Pi ACT LED flash codes — [raspberrypi.com/documentation/computers/configuration.html](https://www.raspberrypi.com/documentation/computers/configuration.html#led-warning-flash-codes)

---

## 15. Quick-reference card

```
Cable:     Raspberry Pi Debug Probe ($12, first-party)
           https://www.raspberrypi.com/products/debug-probe/
           Plugs into the JST-SH "UART" header between the two
           micro-HDMI ports. NOT the 40-pin GPIO header —
           GPIO 14/15 is behind RP1 and does not work yet.

Terminal:  screen /dev/tty.usbmodem* 115200      (macOS, Debug Probe)
           screen /dev/ttyACM0 115200            (Linux, Debug Probe)
           Exit: Ctrl-A, K, y

Build:     make -C kernel/arch/arm64 image
           bash tools/build-pi5-image.sh

SD layout: /bcm2712-rpi-5-b.dtb
           /config.txt
           /kernel_2712.img
           /overlays/bcm2712d0.dtbo   (optional, for rev 1.1 boards)

Flash:     diskutil eraseDisk FAT32 AEGISBOOT MBRFormat /dev/diskN
           cp -rv build/pi5-image/* /Volumes/AEGISBOOT/
           diskutil eject /dev/diskN

Boot:      Insert SD, plug Debug Probe into JST-SH UART header,
           plug Debug Probe USB into laptop, power Pi via USB-C.
           Expect output within 1 second.

Iterate:   Only kernel_2712.img changes between rebuilds. Copy just
           that file to the SD card to save time.

Success:   [BOOT] aegis arm64 (pre-MMU) → [GIC] OK: GICv2 initialized
           → [SCHED] OK → login:
```
