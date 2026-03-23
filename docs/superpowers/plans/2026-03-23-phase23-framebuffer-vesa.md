# Phase 23: Framebuffer Abstraction + VESA/GOP Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a framebuffer abstraction layer with a VESA/GOP backend that parses the multiboot2 framebuffer tag, maps the linear framebuffer into kernel VA, renders 8x16 glyphs, and replaces VGA text-mode output when a framebuffer is available.

**Architecture:** The multiboot2 header gains a framebuffer request tag so GRUB sets up a linear framebuffer. `arch_mm_init()` saves the framebuffer tag info (physical address, width, height, pitch, bpp) into a `boot_fb_t` struct. After VMM init, `fb_vesa_init()` maps the framebuffer physical address into kernel VA. `fb.c` probes AMD DC first (stub, returns 0 in Phase 23), then falls back to VESA. `fb_font.c` contains a static 8x16 font and `fb_blit_glyph()`. `printk.c` gains a framebuffer console path: serial first (Law 2), then FB console if available, else VGA text mode.

**Tech Stack:** C, multiboot2 framebuffer tag (type 8), MMIO framebuffer mapping, 8x16 VGA font, KVA allocator.

**Spec:** docs/superpowers/specs/2026-03-23-aegis-v1-design.md — Phase 23

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/arch/x86_64/boot.asm` | Modify | Add multiboot2 framebuffer request tag to header |
| `kernel/arch/x86_64/arch_mm.c` | Modify | Scan multiboot2 tag type 8, save `boot_fb_t` |
| `kernel/arch/x86_64/arch.h` | Modify | Declare `boot_fb_t` struct, `arch_get_boot_fb()` |
| `kernel/fb/fb.h` | Create | `fb_info_t`, `fb_init()`, `fb_get_info()`, `fb_fill_rect()`, `fb_blit_glyph()` |
| `kernel/fb/fb.c` | Create | Probe backends, console state (row/col, scroll), `fb_console_putchar()` |
| `kernel/fb/fb_vesa.c` | Create | Parse `boot_fb_t`, map framebuffer via KVA, fill `fb_info_t` |
| `kernel/fb/fb_font.c` | Create | Static 8x16 font array (256 glyphs), `fb_blit_glyph()` |
| `kernel/core/printk.c` | Modify | Add FB console output path (serial first, then FB or VGA) |
| `kernel/core/main.c` | Modify | Call `fb_init()` after KVA init |
| `Makefile` | Modify | Add FB_SRCS (fb.c, fb_vesa.c, fb_font.c), create build/kernel/fb/ |
| `tests/expected/boot.txt` | Modify | Add `[FB] OK:` line |
| `tests/test_fb.py` | Create | Verify `[FB] OK:` in serial output |
| `tests/run_tests.sh` | Modify | Wire in test_fb.py |

---

### Task 1: Add framebuffer request tag to boot.asm and scan tag type 8 in arch_mm.c

**Files:**
- Modify: `kernel/arch/x86_64/boot.asm`
- Modify: `kernel/arch/x86_64/arch_mm.c`
- Modify: `kernel/arch/x86_64/arch.h`

- [ ] **Step 1: Update boot.txt (RED — test must fail first)**

Add the following line to `tests/expected/boot.txt` at the appropriate position (after the last existing init line before `[SCHED]` or wherever `fb_init()` will be called in the boot sequence — after KVA init):

```
[FB] OK: no framebuffer tag (text-mode only)
```

This is the expected output on `-machine pc` where GRUB may or may not pass a framebuffer tag depending on the multiboot2 header. On `-machine pc` with `-vga std` and GRUB, if the framebuffer request tag is present, GRUB may set up a framebuffer. However, `make test` uses `-display none -vga std` which provides a VGA device. The exact behavior depends on GRUB's mode selection.

Run `make test` to confirm it fails (the `[FB] OK:` line is missing):

```bash
make test 2>&1 | tail -20
```

- [ ] **Step 2: Add framebuffer request tag to multiboot2 header in boot.asm**

Find the multiboot2 header section in `boot.asm`. After the existing tags (before the end tag), add a framebuffer request tag:

```nasm
; Framebuffer request tag — ask GRUB to set up a linear framebuffer
; Type 5 (MULTIBOOT_HEADER_TAG_FRAMEBUFFER), optional (flags=1)
; Width=0, Height=0, Depth=0 means "any mode GRUB chooses"
align 8
    dw 5            ; type = MULTIBOOT_HEADER_TAG_FRAMEBUFFER
    dw 1            ; flags = optional (bit 0 set = optional)
    dd 20           ; size = 20 bytes
    dd 0            ; width = 0 (any)
    dd 0            ; height = 0 (any)
    dd 0            ; depth = 0 (any)
```

Note: flags=1 (optional) means GRUB will try to set a framebuffer but boot normally if it cannot. This is critical — `make test` must not fail if no framebuffer is available.

- [ ] **Step 3: Add boot_fb_t to arch.h**

```c
/* Framebuffer info saved from multiboot2 tag type 8 */
typedef struct {
    uint64_t addr;      /* physical address of framebuffer */
    uint32_t pitch;     /* bytes per row */
    uint32_t width;     /* pixels */
    uint32_t height;    /* pixels */
    uint8_t  bpp;       /* bits per pixel */
} boot_fb_t;

/* Returns boot framebuffer info. addr==0 if no framebuffer tag found. */
const boot_fb_t *arch_get_boot_fb(void);
```

- [ ] **Step 4: Scan multiboot2 tag type 8 in arch_mm.c**

Add tag type 8 scanning to the existing multiboot2 tag scan loop:

```c
#define MB2_TAG_FRAMEBUFFER 8

typedef struct __attribute__((packed)) {
    uint32_t type;          /* = 8 */
    uint32_t size;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;       /* 1 = RGB */
    uint8_t  reserved;
} mb2_fb_tag_t;

static boot_fb_t s_boot_fb;
```

In the tag scan loop:
```c
        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            const mb2_fb_tag_t *fb = (const mb2_fb_tag_t *)p;
            s_boot_fb.addr   = fb->fb_addr;
            s_boot_fb.pitch  = fb->fb_pitch;
            s_boot_fb.width  = fb->fb_width;
            s_boot_fb.height = fb->fb_height;
            s_boot_fb.bpp    = fb->fb_bpp;
        }
```

Add getter:
```c
const boot_fb_t *arch_get_boot_fb(void)
{
    return &s_boot_fb;
}
```

- [ ] **Step 5: Build**

```bash
make 2>&1 | tail -20
```

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/boot.asm kernel/arch/x86_64/arch_mm.c \
        kernel/arch/x86_64/arch.h tests/expected/boot.txt
git commit -m "arch: add multiboot2 framebuffer request tag and tag type 8 scan"
```

---

### Task 2: Create fb.h interface and fb_font.c (static font + glyph renderer)

**Files:**
- Create: `kernel/fb/fb.h`
- Create: `kernel/fb/fb_font.c`

- [ ] **Step 1: Create fb.h**

```c
/* fb.h — Framebuffer abstraction layer
 *
 * Probes AMD DC first (Phase 24), falls back to VESA/GOP (Phase 23).
 * printk and the shell renderer call only these functions.
 */
#ifndef FB_H
#define FB_H

#include <stdint.h>

typedef struct {
    uint32_t *addr;     /* mapped framebuffer base (kernel VA) */
    uint32_t  width;    /* pixels */
    uint32_t  height;   /* pixels */
    uint32_t  pitch;    /* bytes per row */
    uint8_t   bpp;      /* bits per pixel (32 in practice) */
} fb_info_t;

/* Initialize framebuffer: probe AMD DC, fall back to VESA.
 * Prints [FB] OK to serial. */
void       fb_init(void);

/* Returns framebuffer info, or NULL if no framebuffer available. */
fb_info_t *fb_get_info(void);

/* Fill a rectangle with a solid colour (ARGB8888). */
void       fb_fill_rect(uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t colour);

/* Render a single 8x16 glyph at pixel position (x, y). */
void       fb_blit_glyph(uint32_t x, uint32_t y,
                          uint8_t ch, uint32_t fg, uint32_t bg);

/* Framebuffer text console: putchar with automatic scrolling. */
void       fb_console_putchar(char c);

/* Font dimensions */
#define FB_FONT_WIDTH   8
#define FB_FONT_HEIGHT  16

/* External: 8x16 font data array. font8x16[ch][row] is a byte where
 * bit 7 is the leftmost pixel. */
extern const uint8_t font8x16[256][16];

#endif /* FB_H */
```

- [ ] **Step 2: Create fb_font.c with static 8x16 VGA font**

The standard PC BIOS 8x16 font is public domain. Embed it as a 4096-byte array `font8x16[256][16]`. Each entry is 16 bytes (one byte per row, MSB-first). This is the same font displayed by VGA text mode.

The file will be large (~300 lines of hex data). Generate from the standard CP437 VGA font. Include the printable ASCII range (0x20-0x7E) plus box-drawing characters.

Implement `fb_blit_glyph()` in fb_font.c:

```c
void fb_blit_glyph(uint32_t x, uint32_t y,
                    uint8_t ch, uint32_t fg, uint32_t bg)
{
    fb_info_t *fb = fb_get_info();
    if (!fb || !fb->addr) return;

    uint32_t row, col;
    for (row = 0; row < FB_FONT_HEIGHT; row++) {
        uint8_t bits = font8x16[ch][row];
        uint32_t *pixel = (uint32_t *)((uint8_t *)fb->addr +
                          (y + row) * fb->pitch + x * 4);
        for (col = 0; col < FB_FONT_WIDTH; col++) {
            *pixel++ = (bits & 0x80) ? fg : bg;
            bits <<= 1;
        }
    }
}
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | tail -20
```

Not yet in Makefile — verify existing build is clean.

- [ ] **Step 4: Commit**

```bash
git add kernel/fb/fb.h kernel/fb/fb_font.c
git commit -m "fb: add framebuffer interface (fb.h) and 8x16 VGA font with glyph renderer"
```

---

### Task 3: Create fb_vesa.c and fb.c (probe, fallback, console state)

**Files:**
- Create: `kernel/fb/fb_vesa.c`
- Create: `kernel/fb/fb.c`

- [ ] **Step 1: Create fb_vesa.c**

```c
/* fb_vesa.c — VESA/GOP framebuffer backend
 *
 * Uses multiboot2 framebuffer tag (type 8) passed by GRUB.
 * Maps the physical framebuffer into kernel VA.
 */
#include "fb.h"
#include "arch.h"
#include "printk.h"
#include "vmm.h"
#include "kva.h"
#include <stddef.h>

static fb_info_t s_vesa_fb;

/* Initialize VESA framebuffer from boot_fb_t saved during multiboot2 scan.
 * Returns 1 on success, 0 if no framebuffer tag. */
int fb_vesa_init(void)
{
    const boot_fb_t *boot_fb = arch_get_boot_fb();
    if (boot_fb->addr == 0)
        return 0;   /* no framebuffer tag */

    /* Calculate pages needed for the framebuffer */
    uint64_t fb_size = (uint64_t)boot_fb->pitch * boot_fb->height;
    uint32_t pages = (uint32_t)((fb_size + 0xFFF) >> 12);

    /* Allocate kernel VA range */
    uint64_t va = kva_alloc_pages(pages);
    if (va == 0)
        return 0;

    /* Map each page: framebuffer physical → kernel VA
     * Use write-combining or uncacheable flags for MMIO */
    uint32_t i;
    for (i = 0; i < pages; i++) {
        uint64_t pa = boot_fb->addr + (uint64_t)i * 0x1000;
        vmm_map_page(vmm_get_master_pml4(), va + (uint64_t)i * 0x1000,
                      pa, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NX);
    }

    s_vesa_fb.addr   = (uint32_t *)va;
    s_vesa_fb.width  = boot_fb->width;
    s_vesa_fb.height = boot_fb->height;
    s_vesa_fb.pitch  = boot_fb->pitch;
    s_vesa_fb.bpp    = boot_fb->bpp;

    return 1;
}

fb_info_t *fb_vesa_get_info(void)
{
    if (s_vesa_fb.addr == NULL)
        return NULL;
    return &s_vesa_fb;
}
```

- [ ] **Step 2: Create fb.c**

```c
/* fb.c — Framebuffer abstraction layer
 *
 * Probes AMD DC first (real hardware), falls back to VESA/GOP (QEMU).
 * Manages the scrolling text console state.
 */
#include "fb.h"
#include "printk.h"
#include <stddef.h>
#include <string.h>

/* Forward declarations for backends */
extern int        fb_vesa_init(void);
extern fb_info_t *fb_vesa_get_info(void);

/* Active framebuffer info */
static fb_info_t *s_active_fb = NULL;

/* Console state */
static uint32_t s_con_col = 0;
static uint32_t s_con_row = 0;
static uint32_t s_con_cols = 0;   /* characters per row */
static uint32_t s_con_rows = 0;   /* character rows */

#define FB_FG_COLOR  0x00AAAAAA   /* light grey */
#define FB_BG_COLOR  0x00000000   /* black */

fb_info_t *fb_get_info(void)
{
    return s_active_fb;
}

void fb_fill_rect(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h, uint32_t colour)
{
    if (!s_active_fb || !s_active_fb->addr) return;

    uint32_t row, col;
    for (row = 0; row < h && (y + row) < s_active_fb->height; row++) {
        uint32_t *pixel = (uint32_t *)((uint8_t *)s_active_fb->addr +
                          (y + row) * s_active_fb->pitch + x * 4);
        for (col = 0; col < w && (x + col) < s_active_fb->width; col++) {
            *pixel++ = colour;
        }
    }
}

static void scroll_up(void)
{
    if (!s_active_fb || !s_active_fb->addr) return;

    uint32_t row_bytes = s_active_fb->pitch * FB_FONT_HEIGHT;
    uint32_t total_bytes = s_active_fb->pitch * s_active_fb->height;

    /* Move everything up by one character row */
    memmove(s_active_fb->addr,
            (uint8_t *)s_active_fb->addr + row_bytes,
            total_bytes - row_bytes);

    /* Clear the last row */
    memset((uint8_t *)s_active_fb->addr + total_bytes - row_bytes,
           0, row_bytes);
}

void fb_console_putchar(char c)
{
    if (!s_active_fb) return;

    if (c == '\n') {
        s_con_col = 0;
        s_con_row++;
    } else if (c == '\r') {
        s_con_col = 0;
    } else if (c == '\b') {
        if (s_con_col > 0) s_con_col--;
    } else if (c == '\t') {
        s_con_col = (s_con_col + 8) & ~7;
    } else {
        fb_blit_glyph(s_con_col * FB_FONT_WIDTH,
                       s_con_row * FB_FONT_HEIGHT,
                       (uint8_t)c, FB_FG_COLOR, FB_BG_COLOR);
        s_con_col++;
    }

    if (s_con_col >= s_con_cols) {
        s_con_col = 0;
        s_con_row++;
    }

    while (s_con_row >= s_con_rows) {
        scroll_up();
        s_con_row--;
    }
}

void fb_init(void)
{
    /* Phase 24 will add: if (fb_amddc_probe()) { ... } */

    /* Try VESA/GOP backend */
    if (fb_vesa_init()) {
        s_active_fb = fb_vesa_get_info();
        s_con_cols = s_active_fb->width / FB_FONT_WIDTH;
        s_con_rows = s_active_fb->height / FB_FONT_HEIGHT;
        printk("[FB] OK: %ux%u bpp=%u\n",
               s_active_fb->width, s_active_fb->height, s_active_fb->bpp);
        return;
    }

    /* No framebuffer available — stay on VGA text mode */
    printk("[FB] OK: no framebuffer tag (text-mode only)\n");
}
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | tail -20
```

- [ ] **Step 4: Commit**

```bash
git add kernel/fb/fb_vesa.c kernel/fb/fb.c
git commit -m "fb: add VESA backend and framebuffer console with scrolling"
```

---

### Task 4: Update printk.c and wire fb_init() into main.c

**Files:**
- Modify: `kernel/core/printk.c`
- Modify: `kernel/core/main.c`
- Modify: `Makefile`

- [ ] **Step 1: Add framebuffer path to printk.c**

In the `printk_putchar()` function (or equivalent character output function), add:

```c
#include "fb.h"

static void printk_putchar(char c)
{
    serial_putchar(c);           /* ALWAYS FIRST — Law 2 */
    if (fb_get_info() != NULL)
        fb_console_putchar(c);   /* framebuffer text console */
    else
        vga_putchar(c);          /* text-mode fallback */
}
```

- [ ] **Step 2: Wire fb_init() into main.c**

Call `fb_init()` after KVA init (the framebuffer mapping needs the KVA allocator). Place it before or after subsystems that print to serial — the FB console will start rendering after `fb_init()` returns.

```c
#include "fb.h"

    kva_init();
    fb_init();              /* framebuffer probe — [FB] OK                   */
```

- [ ] **Step 3: Add FB_SRCS to Makefile**

```makefile
FB_SRCS = \
    kernel/fb/fb.c \
    kernel/fb/fb_vesa.c \
    kernel/fb/fb_font.c
```

Add `FB_OBJS` computation and include in the link step. Add `-Ikernel/fb` to CFLAGS. Create the `build/kernel/fb/` directory in the build rules.

- [ ] **Step 4: Build**

```bash
make 2>&1 | tail -20
```

Fix any warnings.

- [ ] **Step 5: Run make test (GREEN)**

```bash
make test 2>&1 | tail -20
```

On `-machine pc` with `-display none -vga std`, GRUB may or may not set a framebuffer depending on whether it honors the optional framebuffer request tag. The `[FB] OK:` line must appear in either case — one of the two messages will be printed.

Verify the boot.txt line matches the actual output. Adjust if GRUB on `-machine pc` does provide a framebuffer tag (in which case the message would be `[FB] OK: 640x480 bpp=32` or similar instead of the text-mode-only message).

- [ ] **Step 6: Commit**

```bash
git add kernel/core/printk.c kernel/core/main.c Makefile
git commit -m "phase23: wire framebuffer into printk and main.c, add FB_SRCS to Makefile"
```

---

### Task 5: Write test_fb.py and wire into run_tests.sh

**Files:**
- Create: `tests/test_fb.py`
- Modify: `tests/run_tests.sh`

- [ ] **Step 1: Create test_fb.py**

Simple test: boot and verify `[FB] OK:` line appears in serial output. Accepts either the framebuffer-active message or the text-mode-only message.

```python
#!/usr/bin/env python3
"""test_fb.py — verify framebuffer init produces [FB] OK: in serial output."""

import subprocess, sys, os

BOOT_TIMEOUT = 120
BUILD = os.path.join(os.path.dirname(__file__), '..', 'build')
ISO = os.path.join(BUILD, 'aegis.iso')

def main():
    if not os.path.exists(ISO):
        print("FAIL: build/aegis.iso not found")
        sys.exit(1)

    qemu_cmd = [
        'qemu-system-x86_64',
        '-machine', 'pc', '-cpu', 'Broadwell',
        '-cdrom', ISO, '-boot', 'order=d',
        '-display', 'none', '-vga', 'std',
        '-nodefaults', '-serial', 'stdio',
        '-no-reboot', '-m', '128M',
        '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
    ]

    proc = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    try:
        stdout, _ = proc.communicate(timeout=BOOT_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()

    output = stdout.decode('utf-8', errors='replace')
    lines = [l for l in output.splitlines() if l.startswith('[')]

    fb_ok = any('[FB] OK:' in l for l in lines)
    if fb_ok:
        print("PASS: framebuffer init OK")
        sys.exit(0)
    else:
        print("FAIL: [FB] OK: not found in serial output")
        for l in lines:
            print(l)
        sys.exit(1)

if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Wire into run_tests.sh**

```bash
echo "--- test_fb ---"
python3 tests/test_fb.py || FAIL=1
```

- [ ] **Step 3: Run**

```bash
python3 tests/test_fb.py
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_fb.py tests/run_tests.sh
git commit -m "tests: add framebuffer init test (test_fb.py)"
```

---

### Task 6: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 23 to Build Status table**

```markdown
| Framebuffer + VESA backend (Phase 23) | ✅ Done | multiboot2 FB tag scan; VESA map; 8x16 font; printk FB path; make test GREEN |
```

- [ ] **Step 2: Add directory layout update**

Add `kernel/fb/` to the directory layout section:

```
kernel/fb/              ← Framebuffer abstraction + backends (VESA, AMD DC)
```

- [ ] **Step 3: Add Phase 23 forward-looking constraints**

```markdown
### Phase 23 forward-looking constraints

**No write-combining on framebuffer mapping.** The framebuffer is mapped with normal caching (no PAT/MTRR configuration). Pixel writes are not write-combined, which is slower than optimal. A future phase may set PAT entries for write-combining on the framebuffer VA range.

**Scroll performance is O(width*height).** `memmove` copies the entire framebuffer minus one row on every scroll. At 1920x1080x32bpp this is ~8MB per scroll. Adequate for a text console; a double-buffer or ring-buffer approach is v2.0 work.

**No cursor rendering.** The text console does not display a blinking cursor. A future phase may XOR a cursor block at the current position.

**GRUB framebuffer mode is "any".** The multiboot2 framebuffer request tag specifies width=0, height=0, depth=0, which lets GRUB pick the mode. On different machines this may be 640x480, 800x600, 1024x768, etc. The font renderer adapts to any resolution.

**VGA text mode is disabled when framebuffer is active.** printk routes to FB console instead of VGA. If the FB mapping is corrupt, serial is the only output path (Law 2 ensures this is safe).
```

- [ ] **Step 4: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md build status for Phase 23"
```

---

## Final Verification

```bash
make test 2>&1 | tail -5
```

Expected: exit 0. The `[FB] OK:` line in boot.txt matches the actual serial output.

```bash
make run
```

Interactive: verify the framebuffer console displays kernel output (if GRUB sets a framebuffer on q35). Serial output should also show all messages.
