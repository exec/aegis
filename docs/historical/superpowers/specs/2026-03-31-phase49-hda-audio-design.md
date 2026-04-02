# Phase 49: HDA Audio — Design Spec

**Date:** 2026-03-31
**Status:** Approved
**Depends on:** Phase 23 (PCIe enumeration), Phase 38 (SMP/IOAPIC)

---

## Overview

Phase 49 adds an Intel High Definition Audio (HDA) controller driver with PCM
playback support. Userspace writes raw PCM samples to `/dev/audio` via `write()`.
The driver handles controller initialization, codec widget graph traversal, DMA
stream management, and volume/mute control. Playback only — no recording in v1.

### Goals

1. **HDA controller driver** — PCI class 04:03, BAR0 MMIO, CORB/RIRB verb engine
2. **Full codec widget graph parser** — enumerate all widgets, trace output path
   from Pin Complex to DAC. Works on any HDA codec (QEMU hda-duplex, Realtek ALC257).
3. **PCM playback** — `/dev/audio` VFS device, fixed 48kHz/16-bit/stereo, double-buffer DMA
4. **Volume/mute** — ioctl interface for SET_AMP_GAIN_MUTE verbs
5. **Automated test** — QEMU WAV capture + Python FFT verification

---

## Component 1: HDA Controller Init

**File:** `kernel/drivers/hda.c`

**PCI discovery:**
```c
const pcie_device_t *dev = pcie_find_device(0x04, 0x03, 0xFF);
```
Class 04 (multimedia), subclass 03 (audio device). Works for Intel HDA controllers
on both QEMU (q35 machine) and real hardware (ThinkPad X13 Gen 1).

**BAR0 mapping:** 4 pages (16KB) via the standard Aegis pattern:
`kva_alloc_pages(4)` → unmap → `vmm_map_page` with `VMM_FLAG_WRITABLE | VMM_FLAG_UCMINUS`.

**Controller reset sequence:**
1. Clear `GCTL.CRST` (bit 0) → wait for it to read 0
2. Set `GCTL.CRST` to 1 → wait for it to read 1
3. Wait 521us for codec enumeration
4. Read `STATESTS` → bit N set means codec N detected

**CORB/RIRB setup:**
- Allocate 1 page each via `kva_alloc_pages(1)`, zero them
- CORB: 256 entries × 4 bytes = 1024 bytes
- RIRB: 256 entries × 8 bytes = 2048 bytes
- Program base addresses from `kva_page_phys()`
- Set sizes to 256 entries (write 0x02 to CORBSIZE/RIRBSIZE)
- Reset pointers (CORBRP bit 15, RIRBWP bit 15)
- Start DMA: set CORBCTL.CORBRUN and RIRBCTL.RIRBDMAEN

**Init printk:** `[HDA] OK: controller v%u.%u, %u codec(s) detected`

---

## Component 2: Codec Verb Engine

**Verb submission** (synchronous polling):
1. Write 32-bit verb to CORB at `(CORBWP + 1) & 255`
2. Increment CORBWP
3. Poll RIRBWP until it advances (response available)
4. Read 8-byte RIRB entry: `[31:0]` = response, `[35:32]` = codec address
5. Return 32-bit response

**Verb format:**
```
Bits [31:28] = CAD (codec address)
Bits [27:20] = NID (node ID)
Bits [19:0]  = Verb + Payload
```

**Helper functions:**
```c
uint32_t hda_verb(uint8_t cad, uint8_t nid, uint32_t verb);
uint32_t hda_get_param(uint8_t cad, uint8_t nid, uint8_t param_id);
void     hda_set_verb12(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t payload);
void     hda_set_verb4(uint8_t cad, uint8_t nid, uint8_t verb, uint16_t payload);
```

**Timeout:** If RIRBWP doesn't advance within 10,000 iterations (~1ms),
return 0 and log a warning. Prevents hangs on broken codecs.

---

## Component 3: Widget Graph Parser

**Full codec enumeration:**
1. Read root node (NID 0) → GET_PARAMETER(0x04) for function group type
2. GET_PARAMETER(0x02) for subnode count → walk all function groups
3. For each audio function group (type 0x01):
   - GET_PARAMETER(0x02) for subnode count → walk all widgets
   - For each widget: GET_PARAMETER(0x09) for audio widget capabilities
   - Store widget type, connection list length, amp capabilities

**Widget types tracked:**

| Type | ID | Description |
|------|----|-------------|
| Audio Output | 0x0 | DAC — source of PCM data |
| Audio Mixer | 0x2 | Mixes multiple inputs |
| Audio Selector | 0x3 | Selects one input from many |
| Pin Complex | 0x4 | Physical jack/speaker output |

**Output path discovery:**
1. Find all Pin Complex widgets
2. For each pin, read Config Default register (verb 0xF1C) → extract default device
   field (bits [23:20]): 0x0=Line Out, 0x1=Speaker, 0x2=HP Out
3. Select the first Speaker or Line Out pin (prefer Speaker for laptop)
4. Trace upstream via connection list: GET_CONN_LIST_ENTRY (verb 0xF02) on the pin
   → follow connections through Mixers/Selectors → find the Audio Output (DAC)
5. Store the path: `{pin_nid, mixer_nid (optional), dac_nid}`

**Fallback:** If graph traversal fails (no speaker pin found), try NID 2 as DAC
and NID 3 as pin (QEMU's fixed layout). If that also fails, print
`[HDA] FAIL: no output path found` and skip audio init.

---

## Component 4: Stream Setup + DMA

**Output stream selection:**
- Read GCAP to determine ISS (input stream count)
- First output stream descriptor is at offset `0x80 + ISS * 0x20`
- Use stream ID 1 (assigned in SDnCTL bits [23:20])

**Stream configuration:**
1. Reset stream: set SDnCTL.SRST (bit 0), wait, clear, wait
2. Set SDnFMT: `0x0011` = 48kHz base, mult=1, div=1, 16-bit, stereo
3. Allocate audio DMA buffer: 8 pages (32KB) via `kva_alloc_pages(8)`
4. Allocate BDL: 1 page, fill 2 entries (double-buffer):
   - Entry 0: phys addr of pages 0-3 (16KB), length=16384, IOC=1
   - Entry 1: phys addr of pages 4-7 (16KB), length=16384, IOC=1
5. Set SDnCBL = 32768 (total buffer size)
6. Set SDnLVI = 1 (last valid BDL index)
7. Write BDL physical address to SDnBDPL/SDnBDPU
8. Set stream ID in SDnCTL

**DAC configuration:**
- SET_CONV_FORMAT on DAC NID: same format as SDnFMT
- SET_STREAM_CHANNEL on DAC NID: stream ID in upper nibble, channel 0

**Pin configuration:**
- SET_PIN_WIDGET_CONTROL: OUT enable (bit 5)
- SET_AMP_GAIN_MUTE: unmute output, set gain to ~75%

**Start playback:** Set SDnCTL.RUN (bit 1) + IOCE (bit 2)

**Buffer layout (double-buffer):**
```
DMA buffer (32KB, 8 pages):
  [0..16383]  = Buffer A (BDL entry 0)
  [16384..32767] = Buffer B (BDL entry 1)

At 48kHz/16-bit/stereo = 192,000 bytes/sec:
  Each 16KB buffer = ~85ms of audio
  Total ring = ~170ms
```

---

## Component 5: `/dev/audio` VFS Device

**Registration:** `audiodev_register()` in `kernel/audio/audiodev.c`, creates a
VFS device node at `/dev/audio` with read/write/close ops.

**audiodev_t struct:**
```c
typedef struct audiodev {
    char     name[16];        /* "audio0" */
    uint32_t sample_rate;     /* 48000 */
    uint8_t  channels;        /* 2 */
    uint8_t  bits_per_sample; /* 16 */
    int    (*write)(struct audiodev *dev, const void *buf, uint32_t len);
    void   (*poll)(struct audiodev *dev);
    void    *priv;
} audiodev_t;
```

**VFS write op:**
1. `copy_from_user` into a kernel bounce buffer (1024 bytes)
2. Copy into the inactive DMA half-buffer at the current write offset
3. If the inactive buffer is full, set a `buffer_ready` flag
4. If both buffers are full, `sched_block()` until the active buffer completes
5. Return bytes written

**VFS ioctl:**
- `AUDIO_SET_VOLUME` (0x4100): payload = gain value 0-127. Sends SET_AMP_GAIN_MUTE
  to the output amp (pin or mixer, whichever has amp capabilities).
- `AUDIO_SET_MUTE` (0x4101): payload = 0 (unmute) or 1 (mute). Sends SET_AMP_GAIN_MUTE
  with mute bit.

**No capability gating for v1.** Any process with VFS_OPEN + VFS_WRITE can play audio.
CAP_KIND_AUDIO deferred to cap table expansion. Document in CLAUDE.md forward constraints.

---

## Component 6: 100Hz Poll + Buffer Management

**`hda_poll()` — called from PIT ISR at 100Hz:**
1. Read SDnSTS — check BCIS (bit 2, buffer completion interrupt status)
2. If BCIS set: write 1 to clear it
3. Advance the active buffer pointer (A→B or B→A)
4. If a writer is blocked (`waiter_task != NULL`), wake it
5. If the now-active buffer has no data ready, fill with silence (zeros) to
   prevent glitches

**Underrun handling:** If `write()` isn't called fast enough and both buffers
are consumed, the driver fills with silence. No crash, no noise — just a gap.
Print a warning to serial: `hda: underrun` (once per occurrence, not per poll).

---

## Component 7: Testing

**QEMU flags:**
```
qemu-system-x86_64 -machine q35 \
  -device intel-hda -device hda-duplex,audiodev=snd0 \
  -audiodev wav,id=snd0,path=build/test_audio.wav
```

**Test binary** (`user/audio_test/main.c`):
1. Open `/dev/audio`
2. Generate 1 second of 440Hz sine wave (48000 samples × 4 bytes = 192KB)
3. Write to fd in chunks
4. Close fd, exit

**Python test** (`tests/test_hda.py`):
1. Boot QEMU q35 with HDA + WAV audiodev
2. Wait for shell, run `/bin/audio_test`
3. Shutdown guest
4. Read `build/test_audio.wav`
5. Verify: valid WAV header, 48kHz/16-bit/stereo
6. FFT the samples, find peak frequency
7. Assert peak is within 430-450Hz (440Hz ± tolerance)
8. Assert RMS > threshold (not silence)

**Boot oracle:** No changes. HDA init line `[HDA] OK: ...` only appears on q35
machines with `-device intel-hda`. The `-machine pc` boot oracle doesn't see it.

---

## Forward Constraints

1. **No recording.** Only playback (output) streams. Capture requires ADC path
   discovery, input stream setup, and a `/dev/audioin` device. Future work.

2. **No capability gating.** `/dev/audio` is accessible to any process. Add
   `CAP_KIND_AUDIO` when the cap table is expanded beyond 16 slots. **Record in
   CLAUDE.md.**

3. **Fixed format.** 48kHz/16-bit/stereo only. No runtime format negotiation.
   Most audio content is 44.1kHz or 48kHz; the driver uses 48kHz (HDA native base
   rate). 44.1kHz content must be resampled in userspace.

4. **Single consumer.** Only one process can write to `/dev/audio` at a time.
   No mixing. A second `open()` succeeds but concurrent writes produce interleaved
   garbage. A userspace audio mixer (PulseAudio-like) is future work.

5. **Polling at 100Hz.** 10ms poll interval is fine for 85ms buffers (8.5× margin).
   But if the system is under heavy load and a poll is delayed >85ms, underrun occurs.
   MSI-X would eliminate this but is not implemented.

6. **Widget graph parser is best-effort.** It finds the first Speaker/Line Out pin
   and traces to a DAC. Complex codec topologies with multiple output paths, EAPD
   control, or power management are not handled. Works for QEMU hda-duplex and
   Realtek ALC257 (ThinkPad X13). Exotic codecs may need manual path configuration.

7. **No jack detection.** Headphone insertion doesn't mute speakers. Pin sense
   (unsolicited response) support is future work.

8. **No power management.** The codec stays powered on. HDA D-state transitions
   are not implemented.

9. **Volume/mute via ioctl only.** No mixer device (`/dev/mixer`). Volume is set
   per-pin or per-mixer amp, whichever has amp capabilities on the discovered path.
   A single gain value (0-127) controls both left and right channels equally.

10. **DMA buffer pages are never freed.** The 8-page audio buffer and 3-page
    control structures (CORB/RIRB/BDL) are allocated at init and kept permanently.
    Acceptable — audio hardware is present for the entire session.
