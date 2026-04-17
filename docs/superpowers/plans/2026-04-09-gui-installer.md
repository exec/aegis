# GUI Installer Implementation Plan (Phase 3 of 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `user/bin/gui-installer/`, a Glyph-based keyboard-driven installer wizard that links libinstall.a (from Phase 2) to perform the actual install. Add a `gui_installer_test.rs` regression gate that drives it through its five screens and verifies the installed disk boots standalone via OVMF.

**Architecture:** Standalone binary that maps the framebuffer via `sys_fb_map` (same pattern as Bastion), creates widgets directly from `user/lib/glyph` (no `glyph_window_t` wrapper — the binary IS the compositor for the duration of the install), drives a five-screen wizard via Tab/Enter keyboard navigation, and on the final confirmation calls `install_run_all` from libinstall.a with progress callbacks that update a `glyph_progress` widget. Emits well-known serial markers (`[LUMEN] installer ready`, `[INSTALLER] screen=N`, `[INSTALLER] step=...`, `[INSTALLER] progress=...`, `[INSTALLER] done`, `[INSTALLER] error=...`) so the test harness can use the same `wait_for_line` + `send_keys` pattern that `login_flow_test` uses to drive Bastion.

**Tech Stack:** C (musl-gcc, dynamic), libinstall.a (Phase 2), libglyph.a, musl's crypt, Aegis sys_fb_map syscall.

**Spec:** `docs/superpowers/specs/2026-04-09-gui-installer-design.md` (sections "GUI installer — wizard layout" and "Testing strategy → gui_installer_test.rs")

**Phase chain:** Phase 1 (installer_test harness) and Phase 2 (libinstall extraction) must both be complete and their tests passing before this phase starts. This is the terminal phase.

---

## File Structure

**Create:**
- `user/bin/gui-installer/main.c` — the wizard (~900 LOC estimate)
- `user/bin/gui-installer/Makefile`
- `rootfs/etc/aegis/caps.d/gui-installer` — capability policy entry
- `tests/tests/gui_installer_test.rs` — regression test

**Modify:**
- `user/lib/glyph/textfield.c` — add password masking (`glyph_textfield_set_mask`)
- `user/lib/glyph/glyph.h` — declare `glyph_textfield_set_mask`
- `Makefile` (top-level) — add gui-installer rule

**Not touched:** kernel, libinstall (from Phase 2 — frozen interface), bastion, lumen, citadel, any other widget.

---

## Task 1: Add password masking to Glyph textfield

**Files:**
- Modify: `user/lib/glyph/glyph.h`
- Modify: `user/lib/glyph/textfield.c`

**Context:** `glyph_textfield_t` currently draws each character literally via `draw_text_ui(surf, tx, ty, tmp, TF_FG)` at `user/lib/glyph/textfield.c:37`. For password fields we want the character buffer preserved but the drawn glyph to be `'*'` (or any caller-chosen mask character). Add a `mask_char` field to the struct (0 = no mask, non-zero = draw that character instead of the real one). Add `glyph_textfield_set_mask(tf, '*')` to enable it.

- [ ] **Step 1: Add the field and setter to `glyph.h`**

Edit `/Users/dylan/Developer/aegis/user/lib/glyph/glyph.h`. Find the textfield struct (around lines 203-210):

```c
typedef struct {
    glyph_widget_t base;
    char buf[256];
    int len;
    int cursor_pos;
    int width_chars;
    void (*on_change)(glyph_widget_t *self, const char *text);
} glyph_textfield_t;
```

Replace with:

```c
typedef struct {
    glyph_widget_t base;
    char buf[256];
    int len;
    int cursor_pos;
    int width_chars;
    char mask_char;  /* 0 = show text, non-zero = render as this char (password) */
    void (*on_change)(glyph_widget_t *self, const char *text);
} glyph_textfield_t;
```

Then find the textfield declarations below (lines 212-214):

```c
glyph_textfield_t *glyph_textfield_create(int width_chars, void (*on_change)(glyph_widget_t *, const char *));
const char *glyph_textfield_get_text(glyph_textfield_t *tf);
void glyph_textfield_set_text(glyph_textfield_t *tf, const char *text);
```

Add a `glyph_textfield_set_mask` declaration immediately after:

```c
glyph_textfield_t *glyph_textfield_create(int width_chars, void (*on_change)(glyph_widget_t *, const char *));
const char *glyph_textfield_get_text(glyph_textfield_t *tf);
void glyph_textfield_set_text(glyph_textfield_t *tf, const char *text);
void glyph_textfield_set_mask(glyph_textfield_t *tf, char mask_char);
```

- [ ] **Step 2: Update `textfield.c` draw function**

Edit `/Users/dylan/Developer/aegis/user/lib/glyph/textfield.c`. Find the per-character drawing loop in `textfield_draw` (around lines 32-38):

```c
    for (int i = 0; i < tf->len; i++) {
        if (tx + cw > ox + self->w - TF_PAD_X)
            break;
        char tmp[2] = { tf->buf[i], '\0' };
        draw_text_ui(surf, tx, ty, tmp, TF_FG);
        tx += cw;
    }
```

Replace with:

```c
    for (int i = 0; i < tf->len; i++) {
        if (tx + cw > ox + self->w - TF_PAD_X)
            break;
        char tmp[2] = { tf->mask_char ? tf->mask_char : tf->buf[i], '\0' };
        draw_text_ui(surf, tx, ty, tmp, TF_FG);
        tx += cw;
    }
```

- [ ] **Step 3: Add the `set_mask` function**

Still editing `/Users/dylan/Developer/aegis/user/lib/glyph/textfield.c`, append after the existing `glyph_textfield_set_text` function (at end of file):

```c
void
glyph_textfield_set_mask(glyph_textfield_t *tf, char mask_char)
{
    if (!tf)
        return;
    tf->mask_char = mask_char;
    glyph_widget_mark_dirty(&tf->base);
}
```

- [ ] **Step 4: Verify Glyph still builds standalone**

```bash
cd /Users/dylan/Developer/aegis/user/lib/glyph && make clean && make
```

Expected: `libglyph.a` rebuilt clean. `mask_char` being zero-initialized by calloc in `glyph_textfield_create` means existing callers get the old behavior automatically.

- [ ] **Step 5: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add user/lib/glyph/glyph.h user/lib/glyph/textfield.c && git commit -m "$(cat <<'EOF'
feat(glyph): add textfield password masking

Adds glyph_textfield_set_mask(tf, '*') which makes subsequent draws
render every character as the mask character instead of the real
buffer contents. Cursor and input handling unchanged — the textfield
still stores the real characters in tf->buf so on_change callbacks
get the true text.

Enables the upcoming GUI installer's password fields.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Scaffold gui-installer — framebuffer init + main loop

**Files:**
- Create: `user/bin/gui-installer/main.c`
- Create: `user/bin/gui-installer/Makefile`
- Create: `rootfs/etc/aegis/caps.d/gui-installer`

**Context:** Bootstrap the binary: map the framebuffer via syscall 513 (same as Bastion's `user/bin/bastion/main.c:416`), allocate a backbuffer, wrap it in a `surface_t`, set up raw keyboard input, install SIGTERM handler for clean exit, emit the `[LUMEN] installer ready` marker (named `[LUMEN]` not `[INSTALLER]` to match Bastion's pattern where `[LUMEN]` is the "graphical session is up" prefix — this way tests use the same wait_for_line pattern), and enter an event loop that dispatches keys to a placeholder screen renderer. Screens themselves are scaffolded in later tasks.

- [ ] **Step 1: Write the minimal `main.c` scaffold**

Write `/Users/dylan/Developer/aegis/user/bin/gui-installer/main.c`:

```c
/* user/bin/gui-installer/main.c — Glyph-based Aegis installer wizard
 *
 * A graphical front-end for libinstall.a.  Maps the framebuffer
 * directly (same pattern as Bastion) and drives a five-screen
 * keyboard-navigable wizard: Welcome → Disk → User → Confirm →
 * Progress.
 *
 * This file owns only the UI. All install logic lives in
 * ../../lib/libinstall/.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <stdint.h>
#include <sys/syscall.h>

#include <glyph.h>
#include "font.h"

#include "libinstall.h"

#define SYS_FB_MAP  513

typedef struct {
    uint64_t addr;
    uint32_t width, height, pitch, bpp;
} fb_info_t;

/* ── Screen enumeration ─────────────────────────────────────────────── */

typedef enum {
    SCREEN_WELCOME = 1,
    SCREEN_DISK    = 2,
    SCREEN_USER    = 3,
    SCREEN_CONFIRM = 4,
    SCREEN_PROGRESS = 5,
} screen_id_t;

/* ── Wizard state ───────────────────────────────────────────────────── */

#define MAX_DISKS 8

typedef struct {
    /* Framebuffer */
    fb_info_t  fb_info;
    uint32_t  *fb;
    uint32_t  *backbuf;
    int        fb_w, fb_h, pitch_px;
    surface_t  surf;

    /* Current screen */
    screen_id_t screen;
    int         dirty;

    /* Disk selection */
    install_blkdev_t disks[MAX_DISKS];
    int              ndisks;
    int              selected_disk;

    /* Credentials */
    char root_pw[64];
    char root_pw_confirm[64];
    char username[64];
    char user_pw[64];
    char user_pw_confirm[64];
    char validation_error[128];

    /* Hashes (computed at confirm time) */
    char root_hash[256];
    char user_hash[256];

    /* Progress (Screen 5) */
    char progress_label[128];
    int  progress_value;
    int  install_done;
    int  install_failed;
    char progress_error[256];
} wizard_state_t;

static wizard_state_t g_st;

/* ── Signal handling ────────────────────────────────────────────────── */

static volatile sig_atomic_t s_term_requested;

static void sigterm_handler(int sig) { (void)sig; s_term_requested = 1; }

/* ── Drawing helpers ────────────────────────────────────────────────── */

static void fill_bg(void)
{
    draw_fill_rect(&g_st.surf, 0, 0, g_st.fb_w, g_st.fb_h, 0x00202030);
}

static void blit(void)
{
    memcpy(g_st.fb, g_st.backbuf,
           (size_t)g_st.pitch_px * g_st.fb_h * 4);
}

static void draw_text14(int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&g_st.surf, g_font_ui, 14, x, y, s, color);
    else
        draw_text_t(&g_st.surf, x, y, s, color);
}

static void draw_text18(int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui)
        font_draw_text(&g_st.surf, g_font_ui, 18, x, y, s, color);
    else
        draw_text_t(&g_st.surf, x, y, s, color);
}

/* ── Screen draw dispatch ───────────────────────────────────────────── */

/* Screen-specific draw/key handlers are added in subsequent tasks.
 * This scaffold just paints a placeholder and transitions on Enter. */

static void draw_placeholder(void)
{
    fill_bg();
    char buf[64];
    snprintf(buf, sizeof(buf), "gui-installer scaffold — screen=%d",
             (int)g_st.screen);
    draw_text18(g_st.fb_w / 2 - 200, g_st.fb_h / 2, buf, 0x00FFFFFF);
    blit();
}

static void render_current_screen(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;
    draw_placeholder();
}

static void handle_key(char c)
{
    /* Scaffold: Enter advances, Escape backs up, Tab is no-op. */
    if (c == '\n' || c == '\r') {
        if (g_st.screen < SCREEN_PROGRESS) {
            g_st.screen++;
            dprintf(2, "[INSTALLER] screen=%d\n", (int)g_st.screen);
            g_st.dirty = 1;
        }
    } else if (c == 27 /* ESC */) {
        if (g_st.screen > SCREEN_WELCOME) {
            g_st.screen--;
            dprintf(2, "[INSTALLER] screen=%d\n", (int)g_st.screen);
            g_st.dirty = 1;
        }
    }
}

/* ── Main ───────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Map framebuffer. */
    memset(&g_st.fb_info, 0, sizeof(g_st.fb_info));
    long fb_rc = syscall(SYS_FB_MAP, &g_st.fb_info);
    if (fb_rc < 0) {
        dprintf(2, "gui-installer: sys_fb_map FAILED (%ld)\n", fb_rc);
        return 1;
    }
    g_st.fb       = (uint32_t *)(uintptr_t)g_st.fb_info.addr;
    g_st.fb_w     = (int)g_st.fb_info.width;
    g_st.fb_h     = (int)g_st.fb_info.height;
    g_st.pitch_px = (int)(g_st.fb_info.pitch / (g_st.fb_info.bpp / 8));

    g_st.backbuf = malloc((size_t)g_st.pitch_px * g_st.fb_h * 4);
    if (!g_st.backbuf) {
        dprintf(2, "gui-installer: backbuffer alloc failed\n");
        return 1;
    }
    g_st.surf = (surface_t){
        .buf   = g_st.backbuf,
        .w     = g_st.fb_w,
        .h     = g_st.fb_h,
        .pitch = g_st.pitch_px,
    };

    /* Fonts */
    font_init();

    /* Raw keyboard */
    struct termios t_orig, t_raw;
    tcgetattr(0, &t_orig);
    t_raw = t_orig;
    t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
    t_raw.c_cc[VMIN]  = 0;
    t_raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t_raw);

    /* SIGTERM → graceful exit */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    /* Initial state */
    g_st.screen = SCREEN_WELCOME;
    g_st.dirty = 1;
    g_st.selected_disk = -1;

    /* Paint first frame and emit readiness marker. */
    render_current_screen();
    dprintf(2, "[LUMEN] installer ready\n");
    dprintf(2, "[INSTALLER] screen=1\n");

    /* Event loop */
    while (!s_term_requested) {
        struct timespec ts = { 0, 16000000 }; /* 16 ms */
        nanosleep(&ts, NULL);

        char c;
        while (read(0, &c, 1) == 1)
            handle_key(c);

        render_current_screen();
    }

    tcsetattr(0, TCSANOW, &t_orig);
    return 0;
}
```

- [ ] **Step 2: Write `Makefile`**

Write `/Users/dylan/Developer/aegis/user/bin/gui-installer/Makefile`:

```makefile
MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -fno-pie -no-pie -Wall -Wl,--build-id=none \
             -I../../lib/glyph \
             -I../../lib/libinstall
TARGET     = gui-installer.elf
GLYPH      = ../../lib/glyph/libglyph.a
LIBINSTALL = ../../lib/libinstall/libinstall.a

$(TARGET): main.c $(GLYPH) $(LIBINSTALL)
	$(CC) $(CFLAGS) -o $@ main.c \
	    -L../../lib/libinstall -linstall \
	    -L../../lib/glyph -lglyph \
	    -lcrypt

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

$(LIBINSTALL):
	$(MAKE) -C ../../lib/libinstall

clean:
	rm -f $(TARGET) *.o
```

- [ ] **Step 3: Write the caps.d policy file**

Write `/Users/dylan/Developer/aegis/rootfs/etc/aegis/caps.d/gui-installer`:

```
admin DISK_ADMIN AUTH FB
```

The `FB` cap is needed for `sys_fb_map` — the text installer doesn't use it so its policy doesn't grant FB. The same two-tier `admin` prefix ensures the caps are only granted after `auth_session`.

- [ ] **Step 4: Wire gui-installer into top-level Makefile**

Edit `/Users/dylan/Developer/aegis/Makefile`. Find the "Programs with extra library dependencies" block (the one that now includes `user/bin/installer/installer.elf` after Phase 2). Add a new rule at the end of that block:

```
user/bin/gui-installer/gui-installer.elf: user/bin/gui-installer/main.c user/lib/glyph/libglyph.a user/lib/libinstall/libinstall.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/gui-installer
```

Also find the `ROOTFS_BIN_SRCS` or equivalent list that enumerates user binaries copied into rootfs.img (search for `installer.elf` in the Makefile to find where it's referenced alongside other user binaries — the exact variable name depends on the current Makefile state). Add `user/bin/gui-installer/gui-installer.elf` to that list so it lands in the live ISO's `/bin/gui-installer`.

If the Makefile uses a generic pattern that globs `user/bin/*/` directories, no further edit is needed — just having the directory in place is enough.

- [ ] **Step 5: Nuclear clean build on fishbowl to confirm the scaffold builds**

```bash
cd /Users/dylan/Developer/aegis && git add user/bin/gui-installer/ rootfs/etc/aegis/caps.d/gui-installer Makefile && git commit -m "$(cat <<'EOF'
feat(gui-installer): scaffold framebuffer + main loop

Creates user/bin/gui-installer/ with a minimal scaffold: maps the
framebuffer via sys_fb_map, allocates a backbuffer, sets up raw
keyboard input, emits [LUMEN] installer ready on first paint, and
runs an event loop that transitions screen state on Enter.

Each screen draws a placeholder; subsequent tasks fill in the
five wizard screens (Welcome, Disk, User, Confirm, Progress).

Adds rootfs/etc/aegis/caps.d/gui-installer with admin-tier
DISK_ADMIN + AUTH + FB caps.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)" && git push origin master && ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -2 && git clean -fdx --exclude=references --exclude=.worktrees 2>&1 | tail -3 && rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf && make iso 2>&1 | tail -5'
```

Expected: ISO built cleanly, including `user/bin/gui-installer/gui-installer.elf`. If the gui-installer's main.c has a compile error (unused variable, missing include, wrong function signature), fix it. The scaffold doesn't exercise all the features yet, so warnings about unused static functions are expected — suppress with `__attribute__((unused))` on them or mark them TODO for later tasks.

---

## Task 3: Screen 1 — Welcome

**Files:**
- Modify: `user/bin/gui-installer/main.c`

**Context:** Replace the `draw_placeholder` call for Screen 1 with a real welcome screen: title "Welcome to Aegis", body text explaining what happens, single `[ Next ]` button that's always keyboard-focused. The button is drawn directly (no `glyph_button_create` yet — we're not using the Glyph button widget for the wizard chrome because we don't need callbacks; Enter always triggers "advance"). The welcome screen is static — no input widgets, no state, just text and a button decoration.

- [ ] **Step 1: Replace `draw_placeholder` with screen-specific draw dispatch**

Edit `/Users/dylan/Developer/aegis/user/bin/gui-installer/main.c`. Find `render_current_screen` (in the scaffold from Task 2) and replace with:

```c
/* Forward declarations for each screen's draw function */
static void draw_screen_welcome(void);
static void draw_screen_disk(void);
static void draw_screen_user(void);
static void draw_screen_confirm(void);
static void draw_screen_progress(void);

static void render_current_screen(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;

    switch (g_st.screen) {
    case SCREEN_WELCOME:  draw_screen_welcome();  break;
    case SCREEN_DISK:     draw_screen_disk();     break;
    case SCREEN_USER:     draw_screen_user();     break;
    case SCREEN_CONFIRM:  draw_screen_confirm();  break;
    case SCREEN_PROGRESS: draw_screen_progress(); break;
    }
    blit();
}
```

Delete the `draw_placeholder` function — it's no longer called.

- [ ] **Step 2: Add a shared chrome helper + the welcome screen**

Still editing `main.c`, add these functions immediately after `render_current_screen`:

```c
/* ── Shared chrome (title + status bar) ─────────────────────────────── */

static void draw_chrome(const char *title)
{
    fill_bg();

    /* Title bar */
    draw_fill_rect(&g_st.surf, 0, 0, g_st.fb_w, 48, 0x002A2A3E);
    draw_text18((g_st.fb_w - (int)strlen(title) * 10) / 2,
                14, title, 0x00FFFFFF);
    draw_fill_rect(&g_st.surf, 0, 48, g_st.fb_w, 1, 0x00404060);

    /* Status bar */
    draw_fill_rect(&g_st.surf, 0, g_st.fb_h - 40,
                   g_st.fb_w, 40, 0x00181828);
    draw_fill_rect(&g_st.surf, 0, g_st.fb_h - 41,
                   g_st.fb_w, 1, 0x00404060);
    char step[32];
    snprintf(step, sizeof(step), "Step %d of 5", (int)g_st.screen);
    draw_text14(20, g_st.fb_h - 26, step, 0x00A0A0B0);
}

static void draw_button(int x, int y, int w, int h,
                        const char *label, int highlighted)
{
    uint32_t bg = highlighted ? 0x005577DD : 0x003A4A6A;
    draw_fill_rect(&g_st.surf, x, y, w, h, bg);
    int tw = (int)strlen(label) * 10;
    draw_text14(x + (w - tw) / 2, y + (h - 18) / 2,
                label, 0x00FFFFFF);
}

/* ── Screen 1: Welcome ──────────────────────────────────────────────── */

static void draw_screen_welcome(void)
{
    draw_chrome("Aegis Installer");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 160, 120, "Welcome to Aegis", 0x00FFFFFF);

    draw_text14(cx - 280, 180,
                "This installer will install Aegis to your disk.",
                0x00C0C0D0);
    draw_text14(cx - 280, 204,
                "All data on the target disk will be erased.",
                0x00C0C0D0);
    draw_text14(cx - 280, 236,
                "Press Enter to continue.",
                0x00808090);

    /* Next button (always highlighted — only button on this screen) */
    draw_button(cx - 50, g_st.fb_h - 120, 100, 40, "Next", 1);
}

/* Temporary stubs — filled in by Tasks 4-7 */
static void draw_screen_disk(void)     { draw_chrome("Select target disk"); }
static void draw_screen_user(void)     { draw_chrome("User account"); }
static void draw_screen_confirm(void)  { draw_chrome("Ready to install"); }
static void draw_screen_progress(void) { draw_chrome("Installing..."); }
```

- [ ] **Step 3: Nuclear clean build on fishbowl**

```bash
cd /Users/dylan/Developer/aegis && git add user/bin/gui-installer/main.c && git commit -m "$(cat <<'EOF'
feat(gui-installer): screen 1 welcome + shared chrome helpers

Adds draw_chrome (title bar + status bar with step counter) and
draw_button (keyboard-focused button sprite). Screen 1 (Welcome)
is fully rendered: title, body text, and a Next button that
always has focus.

Screens 2-5 remain placeholder stubs.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)" && git push origin master && ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -2 && git clean -fdx --exclude=references --exclude=.worktrees 2>&1 | tail -3 && rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf && make iso 2>&1 | tail -5'
```

Expected: clean build.

---

## Task 4: Screen 2 — Disk selection

**Files:**
- Modify: `user/bin/gui-installer/main.c`

**Context:** Enumerate block devices via `install_list_blkdevs()`, filter out ramdisks and partitions (same logic as the text installer at main.c:627), show a scrollable list. Arrow keys change selection, Enter advances, Escape goes back. If only one disk is available, auto-select it and `[ Next ]` advances immediately.

Since the test harness uses `send_keys` which maps `<enter>` to `ret`, selecting the single available disk by pressing Enter is sufficient for the common case (one NVMe in the test).

- [ ] **Step 1: Populate disks when first entering the screen, then draw**

Replace the existing `draw_screen_disk` stub with:

```c
static void screen_disk_populate(void)
{
    /* Populate disk list from libinstall, skipping ramdisks and
     * partitions (anything containing 'p' after the base name). */
    install_blkdev_t raw[MAX_DISKS];
    int n = install_list_blkdevs(raw, MAX_DISKS);
    g_st.ndisks = 0;
    for (int i = 0; i < n && g_st.ndisks < MAX_DISKS; i++) {
        if (strncmp(raw[i].name, "ramdisk", 7) == 0) continue;
        if (strchr(raw[i].name, 'p') != NULL) continue;
        g_st.disks[g_st.ndisks++] = raw[i];
    }
    if (g_st.selected_disk < 0 && g_st.ndisks > 0)
        g_st.selected_disk = 0;
}

static void draw_screen_disk(void)
{
    /* Refresh the disk list each time the screen is entered —
     * cheap enough and handles live-inserted USB etc. */
    if (g_st.ndisks == 0)
        screen_disk_populate();

    draw_chrome("Select target disk");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 200, 96, "Select target disk", 0x00FFFFFF);
    draw_text14(cx - 280, 130,
                "Use Up/Down to change selection, Enter to confirm.",
                0x00A0A0B0);

    /* List of disks */
    int lx = cx - 220;
    int ly = 170;
    int lw = 440;
    int row_h = 32;

    draw_fill_rect(&g_st.surf, lx, ly, lw,
                   row_h * (g_st.ndisks > 0 ? g_st.ndisks : 1) + 8,
                   0x00181828);

    if (g_st.ndisks == 0) {
        draw_text14(lx + 12, ly + 10,
                    "No suitable disk found.", 0x00FF4444);
    } else {
        for (int i = 0; i < g_st.ndisks; i++) {
            int ry = ly + 4 + i * row_h;
            if (i == g_st.selected_disk)
                draw_fill_rect(&g_st.surf, lx + 4, ry - 2,
                               lw - 8, row_h, 0x004488CC);
            char line[64];
            snprintf(line, sizeof(line), "%s  —  %llu MB",
                     g_st.disks[i].name,
                     (unsigned long long)g_st.disks[i].block_count *
                         g_st.disks[i].block_size / (1024 * 1024));
            draw_text14(lx + 12, ry + 8, line, 0x00FFFFFF);
        }
    }

    /* Back + Next buttons */
    draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Back", 0);
    int next_hl = (g_st.ndisks > 0);
    draw_button(cx + 60, g_st.fb_h - 120, 100, 40, "Next", next_hl);
}
```

- [ ] **Step 2: Handle Up/Down keys for the disk list**

Still in `main.c`, modify the `handle_key` function. Find the existing scaffold version from Task 2:

```c
static void handle_key(char c)
{
    if (c == '\n' || c == '\r') {
        if (g_st.screen < SCREEN_PROGRESS) {
            g_st.screen++;
            dprintf(2, "[INSTALLER] screen=%d\n", (int)g_st.screen);
            g_st.dirty = 1;
        }
    } else if (c == 27 /* ESC */) {
        if (g_st.screen > SCREEN_WELCOME) {
            g_st.screen--;
            dprintf(2, "[INSTALLER] screen=%d\n", (int)g_st.screen);
            g_st.dirty = 1;
        }
    }
}
```

Replace with:

```c
static void handle_key_welcome(char c)
{
    if (c == '\n' || c == '\r') {
        g_st.screen = SCREEN_DISK;
        dprintf(2, "[INSTALLER] screen=2\n");
        g_st.dirty = 1;
    }
}

static void handle_key_disk(char c)
{
    /* Arrow keys arrive as 3-byte ANSI escape sequences from stsh's
     * cooked mode passthrough (ESC [ A/B). Our TTY is in raw mode so
     * we'll see all three bytes. Parse a simple state machine. */
    static int arrow_state = 0;
    if (arrow_state == 1) {
        arrow_state = (c == '[') ? 2 : 0;
        return;
    }
    if (arrow_state == 2) {
        if (c == 'A' && g_st.selected_disk > 0) {
            g_st.selected_disk--;
            g_st.dirty = 1;
        } else if (c == 'B' && g_st.selected_disk < g_st.ndisks - 1) {
            g_st.selected_disk++;
            g_st.dirty = 1;
        }
        arrow_state = 0;
        return;
    }
    if (c == 27) {
        /* Might be start of arrow or standalone Escape.
         * Defer the decision one byte. */
        arrow_state = 1;
        return;
    }
    if (c == '\n' || c == '\r') {
        if (g_st.ndisks > 0 && g_st.selected_disk >= 0) {
            g_st.screen = SCREEN_USER;
            dprintf(2, "[INSTALLER] screen=3\n");
            g_st.dirty = 1;
        }
    }
}

/* Temporary stubs — replaced by Task 5/6/7 */
static void handle_key_user(char c)     { if (c == '\n') { g_st.screen = SCREEN_CONFIRM; dprintf(2, "[INSTALLER] screen=4\n"); g_st.dirty = 1; } }
static void handle_key_confirm(char c)  { if (c == '\n') { g_st.screen = SCREEN_PROGRESS; dprintf(2, "[INSTALLER] screen=5\n"); g_st.dirty = 1; } }
static void handle_key_progress(char c) { (void)c; /* no keys during install */ }

static void handle_key(char c)
{
    switch (g_st.screen) {
    case SCREEN_WELCOME:  handle_key_welcome(c);  break;
    case SCREEN_DISK:     handle_key_disk(c);     break;
    case SCREEN_USER:     handle_key_user(c);     break;
    case SCREEN_CONFIRM:  handle_key_confirm(c);  break;
    case SCREEN_PROGRESS: handle_key_progress(c); break;
    }
}
```

**Note:** The arrow key state machine is imperfect — it can miss a standalone Escape if the user types ESC and then waits more than one read(). For this wizard, standalone Escape is unused on Screen 2 (test drives via Enter only). If a future screen needs real Escape support, add a timeout-based terminator.

- [ ] **Step 3: Build + commit**

```bash
cd /Users/dylan/Developer/aegis && git add user/bin/gui-installer/main.c && git commit -m "$(cat <<'EOF'
feat(gui-installer): screen 2 disk selection

Enumerates block devices via install_list_blkdevs, filters out
ramdisks and partitions, renders a list with the current selection
highlighted. Arrow keys (ANSI ESC[A/ESC[B) change selection. Enter
on a valid selection advances to screen 3.

Splits the handle_key dispatch into per-screen handlers so each
task in this plan owns its own key handler.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)" && git push origin master && ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -2 && make iso 2>&1 | tail -5'
```

Expected: clean build.

---

## Task 5: Screen 3 — User account (with masked password fields + validation)

**Files:**
- Modify: `user/bin/gui-installer/main.c`

**Context:** This is the most complex screen. Five input fields (root pw, root pw confirm, username, user pw, user pw confirm) with Tab-cycling focus, masking on the password fields, and inline validation when Enter is pressed on the last field. On validation error, set `g_st.validation_error` and stay on the screen. On success, advance to Screen 4.

We won't use `glyph_textfield_t` here — it's overkill for a simple form and the focus management is simpler with plain char buffers. Re-use the same per-field edit pattern as Bastion's `handle_key`.

- [ ] **Step 1: Add focus state and field editing helpers**

Edit `/Users/dylan/Developer/aegis/user/bin/gui-installer/main.c`. Add these globals immediately after `g_st` (the `static wizard_state_t g_st;` line near the top of the file):

```c
/* Screen 3 focus state. 0=root_pw, 1=root_confirm, 2=username,
 * 3=user_pw, 4=user_confirm, 5=next-button. */
static int s_user_focus = 0;
```

Then add these helpers immediately before `draw_screen_user`:

```c
/* ── Screen 3: User account ─────────────────────────────────────────── */

typedef struct {
    const char *label;
    char       *buf;
    int         buf_sz;
    int         masked;
} user_field_t;

static user_field_t user_fields[5];
static int user_fields_inited = 0;

static void screen_user_init_fields(void)
{
    if (user_fields_inited) return;
    user_fields[0] = (user_field_t){ "Root password",         g_st.root_pw,         sizeof(g_st.root_pw),         1 };
    user_fields[1] = (user_field_t){ "Confirm root password", g_st.root_pw_confirm, sizeof(g_st.root_pw_confirm), 1 };
    user_fields[2] = (user_field_t){ "Username (optional)",   g_st.username,        sizeof(g_st.username),        0 };
    user_fields[3] = (user_field_t){ "User password",         g_st.user_pw,         sizeof(g_st.user_pw),         1 };
    user_fields[4] = (user_field_t){ "Confirm user password", g_st.user_pw_confirm, sizeof(g_st.user_pw_confirm), 1 };
    user_fields_inited = 1;
}

static int screen_user_validate(void)
{
    g_st.validation_error[0] = '\0';
    if (g_st.root_pw[0] == '\0') {
        snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                 "Root password cannot be empty.");
        return -1;
    }
    if (strcmp(g_st.root_pw, g_st.root_pw_confirm) != 0) {
        snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                 "Root passwords do not match.");
        return -1;
    }
    if (g_st.username[0] != '\0') {
        if (g_st.user_pw[0] == '\0') {
            snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                     "User password cannot be empty.");
            return -1;
        }
        if (strcmp(g_st.user_pw, g_st.user_pw_confirm) != 0) {
            snprintf(g_st.validation_error, sizeof(g_st.validation_error),
                     "User passwords do not match.");
            return -1;
        }
    }
    return 0;
}

static void draw_screen_user(void)
{
    screen_user_init_fields();
    draw_chrome("User account");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 100, 72, "User account", 0x00FFFFFF);

    int fx = cx - 220;
    int fy = 120;
    int field_w = 440;
    int field_h = 32;
    int gap     = 16;

    for (int i = 0; i < 5; i++) {
        int y = fy + i * (field_h + gap + 18);
        draw_text14(fx, y, user_fields[i].label, 0x00A0A0B0);
        draw_fill_rect(&g_st.surf, fx, y + 18, field_w, field_h, 0x002A2A3E);
        if (s_user_focus == i)
            draw_rect(&g_st.surf, fx, y + 18, field_w, field_h, 0x004488CC);

        const char *val = user_fields[i].buf;
        int vlen = (int)strlen(val);
        if (user_fields[i].masked && vlen > 0) {
            char stars[64];
            int si;
            for (si = 0; si < vlen && si < 63; si++) stars[si] = '*';
            stars[si] = '\0';
            draw_text14(fx + 8, y + 26, stars, 0x00FFFFFF);
        } else if (vlen > 0) {
            draw_text14(fx + 8, y + 26, val, 0x00FFFFFF);
        }
    }

    /* Back + Next buttons */
    draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Back",
                (s_user_focus == 5 ? 0 : 0)); /* never "focused" to match wireframe */
    draw_button(cx + 60, g_st.fb_h - 120, 100, 40, "Next",
                (s_user_focus == 5));

    /* Validation error message */
    if (g_st.validation_error[0]) {
        int tw = (int)strlen(g_st.validation_error) * 8;
        draw_text14(cx - tw / 2, g_st.fb_h - 160,
                    g_st.validation_error, 0x00FF4444);
    }
}
```

- [ ] **Step 2: Replace the `handle_key_user` stub with the real handler**

Find the existing `handle_key_user` stub from Task 4:

```c
static void handle_key_user(char c)     { if (c == '\n') { g_st.screen = SCREEN_CONFIRM; dprintf(2, "[INSTALLER] screen=4\n"); g_st.dirty = 1; } }
```

Replace with:

```c
static void handle_key_user(char c)
{
    screen_user_init_fields();

    if (c == '\t') {
        /* Tab cycles focus 0..5 */
        s_user_focus = (s_user_focus + 1) % 6;
        g_st.dirty = 1;
        return;
    }
    if (c == '\n' || c == '\r') {
        if (s_user_focus < 5) {
            s_user_focus++;
            g_st.dirty = 1;
            return;
        }
        /* Focus was on Next — validate and advance. */
        if (screen_user_validate() == 0) {
            g_st.validation_error[0] = '\0';
            g_st.screen = SCREEN_CONFIRM;
            dprintf(2, "[INSTALLER] screen=4\n");
        }
        g_st.dirty = 1;
        return;
    }
    if (s_user_focus >= 5) return;

    user_field_t *f = &user_fields[s_user_focus];
    int len = (int)strlen(f->buf);
    if (c == '\b' || c == 127) {
        if (len > 0) {
            f->buf[len - 1] = '\0';
            g_st.dirty = 1;
        }
        return;
    }
    if (c >= ' ' && c < 127 && len < f->buf_sz - 1) {
        f->buf[len]     = c;
        f->buf[len + 1] = '\0';
        g_st.dirty = 1;
    }
}
```

- [ ] **Step 3: Build + commit**

```bash
cd /Users/dylan/Developer/aegis && git add user/bin/gui-installer/main.c && git commit -m "$(cat <<'EOF'
feat(gui-installer): screen 3 user account with masked fields

Five fields: root password, root confirm, username, user password,
user confirm. Tab cycles focus (0-4 = fields, 5 = Next button).
Password fields render as '*' characters. Enter on Next runs
inline validation (non-empty root password, both pairs match) and
either advances to screen 4 or shows an inline red error.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)" && git push origin master && ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -2 && make iso 2>&1 | tail -5'
```

Expected: clean build.

---

## Task 6: Screen 4 — Confirm summary

**Files:**
- Modify: `user/bin/gui-installer/main.c`

**Context:** Display a summary of what's about to happen (target disk, root password status, user account if any). Enter advances to Screen 5 (which starts the install). This is the "last chance to back out" screen.

- [ ] **Step 1: Replace the `draw_screen_confirm` stub**

Find the existing stub:

```c
static void draw_screen_confirm(void)  { draw_chrome("Ready to install"); }
```

Replace with:

```c
static void draw_screen_confirm(void)
{
    draw_chrome("Ready to install");

    int cx = g_st.fb_w / 2;
    draw_text18(cx - 120, 96, "Ready to install", 0x00FFFFFF);

    int lx = cx - 220;
    int y = 150;

    draw_text14(lx, y, "Target disk:", 0x00A0A0B0);
    {
        char line[64];
        if (g_st.selected_disk >= 0 && g_st.selected_disk < g_st.ndisks) {
            snprintf(line, sizeof(line), "%s  (%llu MB)",
                     g_st.disks[g_st.selected_disk].name,
                     (unsigned long long)g_st.disks[g_st.selected_disk]
                         .block_count *
                         g_st.disks[g_st.selected_disk].block_size /
                         (1024 * 1024));
        } else {
            snprintf(line, sizeof(line), "(none)");
        }
        draw_text14(lx + 180, y, line, 0x00FFFFFF);
    }
    y += 30;

    draw_text14(lx, y, "Root password:", 0x00A0A0B0);
    draw_text14(lx + 180, y, "set", 0x00FFFFFF);
    y += 30;

    draw_text14(lx, y, "User account:", 0x00A0A0B0);
    draw_text14(lx + 180, y,
                g_st.username[0] ? g_st.username : "(none)",
                0x00FFFFFF);
    y += 60;

    draw_text14(lx, y,
                "WARNING: all existing data on the target disk will be erased.",
                0x00FFAA40);

    /* Back + Install buttons.  Install is the default focus. */
    draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Back", 0);
    draw_button(cx + 60, g_st.fb_h - 120, 100, 40, "Install", 1);
}
```

- [ ] **Step 2: The existing `handle_key_confirm` stub already advances on Enter — keep it as-is**

The stub from Task 4 is:

```c
static void handle_key_confirm(char c)  { if (c == '\n') { g_st.screen = SCREEN_PROGRESS; dprintf(2, "[INSTALLER] screen=5\n"); g_st.dirty = 1; } }
```

This is sufficient for the confirm screen — Enter advances to progress. No change needed.

- [ ] **Step 3: Build + commit**

```bash
cd /Users/dylan/Developer/aegis && git add user/bin/gui-installer/main.c && git commit -m "$(cat <<'EOF'
feat(gui-installer): screen 4 confirm summary

Shows target disk + partition size, root password status, optional
user account. Enter on the Install button (default focus) advances
to the progress screen.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)" && git push origin master && ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -2 && make iso 2>&1 | tail -5'
```

Expected: clean build.

---

## Task 7: Screen 5 — Progress + done/error states

**Files:**
- Modify: `user/bin/gui-installer/main.c`

**Context:** When Screen 5 is first entered, hash the passwords (libinstall's `install_hash_password`) and kick off `install_run_all` synchronously. Progress callbacks update `g_st.progress_label`, `g_st.progress_value`, and repaint the screen. After completion, show either "Installation complete — [Reboot]" or "Installation failed — [Retry] [Abort]".

**Threading note:** libinstall's `install_run_all` is synchronous and blocks the caller. We run it inline on the main thread, which means the framebuffer doesn't update while it's running. For the test that's fine — the test waits for `[INSTALLER] done` (serial marker) which fires when the function returns. For interactive use on slow disks, progress callbacks redraw-in-place, so the screen updates as phases advance (each `on_step` callback blocks briefly, renders, blits).

The alternative is forking or a background thread. Too much scope for this phase — stick with synchronous + in-callback repaint.

- [ ] **Step 1: Add install execution + progress state machine**

Edit `/Users/dylan/Developer/aegis/user/bin/gui-installer/main.c`. Add this near the top, right after the `wizard_state_t` struct:

```c
/* Set to 1 once the install has been kicked off on Screen 5,
 * so we don't re-run it on redraws. */
static int s_install_started = 0;
```

Add these helpers right before `draw_screen_progress`:

```c
/* ── Progress callbacks (call back into the UI from libinstall) ─────── */

static void prog_on_step(const char *label, void *ctx)
{
    (void)ctx;
    snprintf(g_st.progress_label, sizeof(g_st.progress_label),
             "%s", label);
    g_st.progress_value = 0;
    dprintf(2, "[INSTALLER] step=%s\n", label);
    /* Repaint immediately so the user sees phase transitions. */
    g_st.dirty = 1;
    render_current_screen();
}

static void prog_on_progress(int pct, void *ctx)
{
    (void)ctx;
    g_st.progress_value = pct;
    if (pct % 10 == 0 || pct == 100)
        dprintf(2, "[INSTALLER] progress=%d\n", pct);
    g_st.dirty = 1;
    render_current_screen();
}

static void prog_on_error(const char *msg, void *ctx)
{
    (void)ctx;
    snprintf(g_st.progress_error, sizeof(g_st.progress_error),
             "%s", msg);
    dprintf(2, "[INSTALLER] error=%s\n", msg);
    /* install_run_all returns -1 after calling us, which the caller
     * below turns into install_failed = 1. */
}

static void run_install(void)
{
    if (s_install_started) return;
    s_install_started = 1;

    /* Hash passwords */
    if (install_hash_password(g_st.root_pw,
                              g_st.root_hash,
                              sizeof(g_st.root_hash)) < 0) {
        snprintf(g_st.progress_error, sizeof(g_st.progress_error),
                 "failed to hash root password");
        dprintf(2, "[INSTALLER] error=%s\n", g_st.progress_error);
        g_st.install_failed = 1;
        return;
    }
    if (g_st.username[0]) {
        if (install_hash_password(g_st.user_pw,
                                  g_st.user_hash,
                                  sizeof(g_st.user_hash)) < 0) {
            snprintf(g_st.progress_error, sizeof(g_st.progress_error),
                     "failed to hash user password");
            dprintf(2, "[INSTALLER] error=%s\n", g_st.progress_error);
            g_st.install_failed = 1;
            return;
        }
    }

    install_progress_t p = {
        .on_step     = prog_on_step,
        .on_progress = prog_on_progress,
        .on_error    = prog_on_error,
        .ctx         = NULL,
    };

    int rc = install_run_all(
        g_st.disks[g_st.selected_disk].name,
        g_st.disks[g_st.selected_disk].block_count,
        g_st.root_hash,
        g_st.username[0] ? g_st.username     : NULL,
        g_st.username[0] ? g_st.user_hash    : NULL,
        &p);

    if (rc == 0) {
        g_st.install_done = 1;
        dprintf(2, "[INSTALLER] done\n");
    } else {
        g_st.install_failed = 1;
    }
    g_st.dirty = 1;
}
```

- [ ] **Step 2: Replace the `draw_screen_progress` stub**

Find:

```c
static void draw_screen_progress(void) { draw_chrome("Installing..."); }
```

Replace with:

```c
static void draw_screen_progress(void)
{
    /* On first draw of Screen 5, run the install synchronously. */
    if (!s_install_started)
        run_install();

    const char *title;
    if (g_st.install_done)        title = "Installation complete";
    else if (g_st.install_failed) title = "Installation failed";
    else                          title = "Installing...";

    draw_chrome(title);
    int cx = g_st.fb_w / 2;

    draw_text18(cx - 160, 96, title, 0x00FFFFFF);

    /* Current phase label */
    if (g_st.progress_label[0])
        draw_text14(cx - 280, 150, g_st.progress_label, 0x00C0C0D0);

    /* Progress bar */
    int bar_x = cx - 240;
    int bar_y = 184;
    int bar_w = 480;
    int bar_h = 24;
    draw_fill_rect(&g_st.surf, bar_x, bar_y, bar_w, bar_h, 0x00181828);
    int fill_w = bar_w * g_st.progress_value / 100;
    if (fill_w > 0)
        draw_fill_rect(&g_st.surf, bar_x, bar_y, fill_w, bar_h, 0x004488CC);
    char pct_txt[8];
    snprintf(pct_txt, sizeof(pct_txt), "%d%%", g_st.progress_value);
    draw_text14(cx - 16, bar_y + bar_h + 10, pct_txt, 0x00A0A0B0);

    /* Error message (on failure) */
    if (g_st.install_failed && g_st.progress_error[0]) {
        draw_text14(cx - 280, 260, "Error:", 0x00FF4444);
        draw_text14(cx - 280, 284, g_st.progress_error, 0x00FF8888);
    }

    /* Buttons — only visible once the run has completed. */
    if (g_st.install_done) {
        draw_button(cx - 50, g_st.fb_h - 120, 100, 40, "Reboot", 1);
    } else if (g_st.install_failed) {
        draw_button(cx - 160, g_st.fb_h - 120, 100, 40, "Retry", 0);
        draw_button(cx + 60,  g_st.fb_h - 120, 100, 40, "Abort", 1);
    }
}
```

- [ ] **Step 3: Replace the `handle_key_progress` stub with reboot/retry/abort handling**

Find:

```c
static void handle_key_progress(char c) { (void)c; /* no keys during install */ }
```

Replace with:

```c
static void handle_key_progress(char c)
{
    if (!g_st.install_done && !g_st.install_failed)
        return;  /* install still running, ignore keys */

    if (c == '\n' || c == '\r') {
        if (g_st.install_done) {
            /* Reboot — sys_reboot(1) matches Lumen's menu behavior. */
            syscall(SYS_REBOOT, 1L);
            /* If reboot fails, fall through. */
            _exit(0);
        } else if (g_st.install_failed) {
            /* Retry: reset state and go back to the confirm screen. */
            s_install_started = 0;
            g_st.install_failed = 0;
            g_st.progress_value = 0;
            g_st.progress_label[0] = '\0';
            g_st.progress_error[0] = '\0';
            g_st.screen = SCREEN_CONFIRM;
            dprintf(2, "[INSTALLER] screen=4\n");
            g_st.dirty = 1;
        }
    } else if (c == 27) {
        /* Escape on failure = Abort */
        if (g_st.install_failed)
            _exit(1);
    }
}
```

Add `#define SYS_REBOOT 306` near the top of the file (next to `#define SYS_FB_MAP 513`). The real syscall number is what the existing `sys_reboot` uses in Lumen — search `grep -n SYS_REBOOT user/bin/lumen/main.c` if uncertain. If 306 is wrong, look at the existing Lumen reboot call site; the comment there typically documents the number.

- [ ] **Step 4: Build + commit**

```bash
cd /Users/dylan/Developer/aegis && git add user/bin/gui-installer/main.c && git commit -m "$(cat <<'EOF'
feat(gui-installer): screen 5 progress + done/error states

Runs install_run_all synchronously with progress callbacks that
repaint the screen on every step/progress update. Emits the full
set of [INSTALLER] serial markers (step=, progress=, done, error=)
plus [LUMEN] installer ready on first paint.

Done state shows a Reboot button (sys_reboot(1) on Enter). Error
state shows Retry (goes back to confirm) and Abort (exits 1).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)" && git push origin master && ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -2 && make iso 2>&1 | tail -5'
```

Expected: clean build. The full gui-installer binary now exists and does everything the text installer does, just with a GUI.

---

## Task 8: Write `gui_installer_test.rs` end-to-end

**Files:**
- Create: `tests/tests/gui_installer_test.rs`

**Context:** Two-boot structure like `installer_test.rs`, but boot 1 drives the GUI installer via keyboard instead of the text installer via CLI prompts. On the live ISO, the GUI installer is invoked through stsh just like the text one (`gui-installer\n`). It opens directly on the framebuffer, takes over the screen, and emits `[LUMEN] installer ready`.

- [ ] **Step 1: Write the test file**

Write `/Users/dylan/Developer/aegis/tests/tests/gui_installer_test.rs`:

```rust
// End-to-end GUI installer regression test.
//
// Mirrors installer_test.rs but drives the graphical wizard via
// Tab/Enter keyboard navigation instead of the text installer's
// CLI prompts.
//
// Boot 1 flow:
//   1. Live ISO + empty NVMe
//   2. Log into stsh as root
//   3. Launch /bin/gui-installer
//   4. Wait for [LUMEN] installer ready + [INSTALLER] screen=1
//   5. Press Enter → [INSTALLER] screen=2 (disk selection)
//   6. Press Enter → [INSTALLER] screen=3 (user account)
//   7. Type root password twice, Tab to username, type username,
//      Tab to user pw, type twice, Tab to Next, Enter
//   8. Wait for [INSTALLER] screen=4 → Enter → screen=5
//   9. Wait for [INSTALLER] done
//
// Boot 2 flow: OVMF boot from installed disk, verify
// [BASTION] greeter ready, same as installer_test.rs.
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test gui_installer_test -- --nocapture

use aegis_tests::{
    aegis_q35_installed_ovmf, aegis_q35_installer, iso, wait_for_line,
    AegisHarness,
};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const DISK_SIZE_MB: u64 = 128;
const BOOT_TIMEOUT_SECS: u64 = 120;
const INSTALL_TIMEOUT_SECS: u64 = 120;

const ROOT_PW: &str = "forevervigilant";
const USER_NAME: &str = "alice";
const USER_PW: &str = "alicepass";

fn disk_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/gui_installer_test_disk.img")
}

fn ovmf_path() -> PathBuf {
    PathBuf::from("/usr/share/OVMF/OVMF_CODE_4M.fd")
}

fn make_fresh_disk(path: &std::path::Path) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let _ = std::fs::remove_file(path);
    let status = Command::new("truncate")
        .arg(format!("-s{}M", DISK_SIZE_MB))
        .arg(path)
        .status()?;
    if !status.success() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::Other,
            format!("truncate failed with status {}", status),
        ));
    }
    Ok(())
}

#[tokio::test]
async fn gui_install_and_boot_from_nvme() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }
    if !ovmf_path().exists() {
        eprintln!("SKIP: OVMF not found at {}", ovmf_path().display());
        return;
    }

    let disk = disk_path();
    make_fresh_disk(&disk).expect("create fresh disk");
    eprintln!("fresh disk: {} ({} MB)", disk.display(), DISK_SIZE_MB);

    // ── Boot 1: GUI installer ────────────────────────────────────────
    eprintln!("==> Boot 1: live ISO + empty NVMe (GUI install)");
    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_installer(&disk), &iso)
            .await
            .expect("boot 1 spawn failed");

    // Wait for stsh login prompt (live ISO auto-logs root into stsh).
    wait_for_line(&mut stream, "# ",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 1: stsh prompt");
    tokio::time::sleep(Duration::from_millis(300)).await;

    // Launch the GUI installer.
    proc.send_keys("gui-installer\n")
        .await
        .expect("sendkey gui-installer");

    // Wait for the wizard to come up.
    wait_for_line(&mut stream, "[LUMEN] installer ready",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: GUI installer ready");
    wait_for_line(&mut stream, "[INSTALLER] screen=1",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 1 marker");

    // ── Screen 1 → Screen 2 (Welcome → Disk) ──
    proc.send_keys("\n").await.expect("sendkey welcome-next");
    wait_for_line(&mut stream, "[INSTALLER] screen=2",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 2 marker");

    // ── Screen 2 → Screen 3 (Disk → User) ──
    // Only one disk (nvme0) is available; Enter confirms.
    proc.send_keys("\n").await.expect("sendkey disk-next");
    wait_for_line(&mut stream, "[INSTALLER] screen=3",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 3 marker");

    // ── Screen 3 → Screen 4 (User → Confirm) ──
    // Five fields: root_pw, root_confirm, username, user_pw, user_confirm,
    // then Tab once more lands on Next, then Enter.
    // Focus starts at field 0 (root_pw).
    //
    // Type root password → Tab → type confirm → Tab → type username
    // → Tab → type user password → Tab → type user confirm → Tab
    // (now on Next) → Enter.
    proc.send_keys(&format!("{}\t{}\t{}\t{}\t{}\t\n",
                            ROOT_PW, ROOT_PW,
                            USER_NAME,
                            USER_PW, USER_PW))
        .await
        .expect("sendkey user form");
    wait_for_line(&mut stream, "[INSTALLER] screen=4",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 4 marker");

    // ── Screen 4 → Screen 5 (Confirm → Progress) ──
    proc.send_keys("\n").await.expect("sendkey confirm-install");
    wait_for_line(&mut stream, "[INSTALLER] screen=5",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 5 marker");

    // Wait for install completion.
    wait_for_line(&mut stream, "[INSTALLER] done",
                  Duration::from_secs(INSTALL_TIMEOUT_SECS))
        .await
        .expect("boot 1: installation did not complete in time");
    eprintln!("    installation complete");

    // GUI installer is now showing "Reboot" — we could press Enter
    // to trigger sys_reboot, but killing the process directly is
    // simpler and avoids any reboot-path side effects for the test.
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.kill().await.expect("boot 1 kill");
    drop(stream);

    // ── Boot 2: OVMF + installed NVMe ────────────────────────────────
    eprintln!("==> Boot 2: OVMF + installed NVMe");
    let (mut stream2, mut proc2) =
        AegisHarness::boot_disk_only(
            aegis_q35_installed_ovmf(&disk, &ovmf_path()),
        )
        .await
        .expect("boot 2 spawn failed");

    wait_for_line(&mut stream2, "[EXT2] OK: mounted nvme0p1",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 2: ext2 mount");
    wait_for_line(&mut stream2, "[BASTION] greeter ready",
                  Duration::from_secs(30))
        .await
        .expect("boot 2: bastion greeter");

    proc2.kill().await.expect("boot 2 kill");
    eprintln!("PASS: gui_install_and_boot_from_nvme");
}
```

- [ ] **Step 2: `cargo check` the test**

```bash
cd /Users/dylan/Developer/aegis && cargo check --manifest-path tests/Cargo.toml --tests
```

Expected: clean. All imports resolve from Phase 1's harness additions.

- [ ] **Step 3: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add tests/tests/gui_installer_test.rs && git commit -m "$(cat <<'EOF'
test: add gui_installer_test regression gate

Drives the gui-installer wizard through all five screens via
Tab+Enter keyboard navigation, then verifies the installed disk
boots standalone via OVMF UEFI (same second-boot pattern as
installer_test.rs).

Uses the serial markers emitted by gui-installer:
  [LUMEN] installer ready
  [INSTALLER] screen=1..5
  [INSTALLER] done

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Run everything on fishbowl and fix what breaks

**Files:** none — verification + fix pass.

**Context:** GUI wizards are fiddly. The first end-to-end run will probably uncover at least one of: (a) field focus off-by-one, (b) password masking showing plaintext, (c) progress callbacks firing too fast and the test missing the `screen=5` marker, (d) Enter on Screen 3 hitting the "submit" path early because the focus cycle is wrong.

- [ ] **Step 1: Push and run the gui_installer_test**

```bash
cd /Users/dylan/Developer/aegis && git push origin master && ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -2 && git clean -fdx --exclude=references --exclude=.worktrees 2>&1 | tail -3 && rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf && make iso 2>&1 | tail -3 && AEGIS_ISO=$PWD/build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test gui_installer_test -- --nocapture 2>&1 | tail -40'
```

- [ ] **Step 2: If the test fails, iterate**

Likely failure modes:

**`boot 1: GUI installer ready` timeout.** The binary might not be in `/bin` of the live ISO, or `sys_fb_map` might be failing because the binary's caps.d file isn't being loaded. Verify:
```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'grep -r gui-installer ~/Developer/aegis/build/isodir/ 2>&1 | head'
```
If gui-installer.elf isn't in the ISO, the top-level Makefile's rootfs packaging step is missing it — check how `installer.elf` gets copied and replicate for gui-installer.elf.

**`boot 1: screen 3 marker` missing.** Screen 3 is entered after Enter on screen 2. If it doesn't fire: the `handle_key_disk` Enter case isn't setting `g_st.screen = SCREEN_USER`. Check for a bug where `g_st.ndisks == 0` because `install_list_blkdevs` returns empty (maybe the caps aren't right for the syscall).

**User form key sequence submits too early.** The test sends `"pw\tpw\tuser\tpw\tpw\t\n"` — 5 tabs + final Enter. With 6 focus positions (0=root_pw, 1=root_confirm, 2=username, 3=user_pw, 4=user_confirm, 5=Next-button), 5 tabs moves focus from 0 → 5 (Next). That assumes the Enter-advances-field behavior in `handle_key_user` doesn't activate first. Re-read the handler — if Enter in field 0 advances to field 1 instead of submitting, the test string has too many separators. Count: `forevervigilant\tforevervigilant\talice\talicepass\talicepass\t\n` = 5 tabs + 1 newline. Each `\t` increments focus by 1. Starting at 0, after 5 tabs focus is 5 (Next). Then `\n` submits. ✅ correct.

BUT: when we type the root password, each character is a keystroke routed to the focused field. If `handle_key_user` processes Enter (`\n`) in field 0 as "advance to field 1" instead of "insert newline", that's fine. Our handler has: `if (c == '\n' || c == '\r') { if (s_user_focus < 5) { s_user_focus++; ... return; } ... }`. So a `\n` in field 0 sets focus = 1 then returns. That's different from Tab (`\t`). So the test should use `\t` not `\n` to move between fields, which it does. ✅

**Install phase hangs or times out.** Progress callbacks are running synchronously on the main thread. If they take too long to emit (e.g. the 4MB copy takes 30s and we're in the middle of one `copy_blocks_internal` call), no serial output happens for 30s. The test's 120s timeout for `[INSTALLER] done` should cover this. If it doesn't, bump to 180s.

**Progress bar not updating during install.** Cosmetic — test doesn't check this. Skip for now.

Fix, commit, rerun. Each iteration is its own commit.

- [ ] **Step 3: Run the entire test suite on fishbowl for a final green pass**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && AEGIS_ISO=$PWD/build/aegis.iso cargo test --manifest-path tests/Cargo.toml -- --nocapture 2>&1 | grep -E "test result|finished"'
```

Expected: all 7 test files pass:
- boot_oracle
- dock_click_test
- gui_installer_test
- installer_test
- login_flow_test
- mouse_api_smoke_test
- screendump_test

- [ ] **Step 4: No commit — verification only**

---

## Self-Review

**Spec coverage:**
- "`user/bin/gui-installer/`" — Tasks 2-7. ✅
- "5 keyboard-driven screens (Welcome → Disk → User → Confirm → Progress)" — Tasks 3-7. ✅
- "Reusable Glyph widgets" — **deviation:** the plan uses direct drawing via `draw_fill_rect` / `draw_text` instead of `glyph_button_t` / `glyph_textfield_t`. Reason: the wizard chrome only needs visuals, not widget callbacks, and focus management is simpler with explicit `s_user_focus` state. The only Glyph dependency is `draw.h` (pixel primitives) and `font.h` (text rendering). `glyph_textfield_set_mask` is still added in Task 1 because it's a reusable library improvement, even though the GUI installer doesn't use `glyph_textfield_t` directly. **Note:** flag this deviation from the spec; the spec said "Reuses Bastion's credential fields pattern" which Bastion itself does the same way (draw_fill_rect + manual focus), so we're actually following the Bastion precedent more faithfully than the spec's paragraph about `glyph_textfield`. ✅ (intentional deviation, consistent with project norms)
- "Serial markers: [LUMEN] installer ready, [INSTALLER] screen=N, step=, progress=, done, error=" — Task 7. ✅
- "install_run_all orchestrator call from Screen 5" — Task 7 `run_install`. ✅
- "Keyboard-driven so tests drive via send_keys" — Tasks 3-6 handle Tab/Enter/ESC/Up/Down. ✅
- "Password masking in Glyph textfield" — Task 1 `glyph_textfield_set_mask`. ✅
- "Error display in Screen 5" — Task 7 draws red error text below progress bar. ✅
- "Reboot button action calls sys_reboot(1)" — Task 7 `handle_key_progress`. ✅
- "rootfs/etc/aegis/caps.d/gui-installer" — Task 2. ✅
- "`gui_installer_test.rs`" — Task 8. ✅

**Placeholder scan:** no TBDs. Every step has a concrete code block or command. The arrow-key state machine in Task 4 has a comment noting its imperfection, but it's a known limitation with a documented edge case (standalone Escape not supported on Screen 2), not a placeholder. ✅

**Type consistency:**
- `wizard_state_t` has stable fields across tasks. `screen_id_t` enum values are used consistently. ✅
- `install_progress_t` (from libinstall.h) matches the struct declared in Phase 2's plan. ✅
- Glyph function signatures (`draw_fill_rect`, `draw_text14`, etc.) match the project's existing usage in Bastion. ✅
- `install_list_blkdevs`, `install_run_all`, `install_hash_password` signatures match Phase 2 exactly. ✅

**Open correctness risks:**
1. `SYS_REBOOT` syscall number is guessed at 306. Verify at implementation time by grepping `SYS_REBOOT` in the kernel's `syscall_dispatch` table or in `user/bin/lumen/main.c`. Task 7 Step 3 notes this.
2. The `s_install_started` flag is a global — if the test hits Retry and the code re-runs `run_install`, `s_install_started` is reset to 0 but any other state might not be. Task 7's retry path resets `g_st.install_failed`, `progress_value`, `progress_label`, `progress_error` explicitly. ✅
3. Arrow key state machine can get stuck on partial sequences if the ANSI escape arrives split across `read()` calls. For the test this is unlikely because `send_keys` delivers keys with 40ms gaps and a single `read` returns at most 1 byte at a time. ✅
4. The GUI installer binary needs to be in the live ISO's `/bin` directory. Task 2 Step 4 covers this by adding the Makefile rule, but if the rootfs packaging uses a different mechanism (a whitelist file, a wildcard pattern), verify at implementation time that `gui-installer.elf` lands in the ISO.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-09-gui-installer.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Prerequisites: Phase 1 (`2026-04-09-installer-test-harness.md`) and Phase 2 (`2026-04-09-libinstall-extraction.md`) must both be complete and their tests passing before this phase starts. This is the terminal phase — after it's green, the project ships Phase 47 (GUI installer) per the roadmap.
