# Phase 37: Lumen — Display Compositor

## Goal

Build Lumen, the Aegis display compositor. A single userspace process that owns the framebuffer, manages windows with z-ordering, dispatches keyboard/mouse input to the focused window, renders a software cursor, and draws basic window chrome. In v0.1, Lumen and Citadel (the desktop shell) are a single binary — no IPC, no multi-process client protocol.

## Scope

1. **Compositor core** — framebuffer ownership via `sys_fb_map`, event loop with `epoll_wait`, window management
2. **Window system** — z-ordered window list (16 max), click-to-focus with raise, titlebar drag, close button
3. **Window chrome** — title bar with gradient, close/minimize/maximize buttons, 1px border, drop shadow
4. **Software cursor** — save-under technique, 16x16 monochrome arrow sprite baked as C array
5. **Input dispatch** — keyboard events to focused window, mouse events for compositor actions (drag, focus, resize) and client forwarding
6. **Desktop** — gradient background (same as fb_test), taskbar with clock and app launcher button
7. **Built-in windows** — a terminal emulator window (reads kbd, displays shell output via PTY), a system info window (static, like fb_test)
8. **Terminus 10x20 font** — reuse existing `terminus20.h` from fb_test

## Non-Goals

- Multi-process clients (v0.2 — requires MAP_SHARED + fd passing IPC)
- Window resizing (complex client notification + buffer realloc)
- Transparency / alpha blending
- Animations / transitions
- Multiple monitors
- GPU acceleration
- libglyph.so (Phase 38)
- Anti-aliased fonts

---

## 1. Architecture

Lumen is a single dynamically-linked userspace binary at `/bin/lumen`. It is launched by the desktop session after login (vigil service or manual invocation). It runs in a loop until the user logs out or the system shuts down.

```
┌──────────────────────────────────────────────┐
│              Physical Framebuffer             │
│          (mapped via sys_fb_map 513)          │
├──────────────────────────────────────────────┤
│              Lumen process                    │
│  ┌─────────────┐  ┌────────────────────────┐ │
│  │ Event loop   │  │ Compositor             │ │
│  │ (epoll on    │  │ - window list (z-order)│ │
│  │  kbd+mouse)  │  │ - chrome renderer      │ │
│  │              │  │ - cursor (save-under)  │ │
│  │              │  │ - background           │ │
│  └──────┬───────┘  └────────────┬───────────┘ │
│         │                       │             │
│  ┌──────┴───────────────────────┴───────────┐ │
│  │            Built-in Windows               │ │
│  │  ┌──────────┐ ┌────────────┐ ┌─────────┐ │ │
│  │  │ Terminal  │ │ Sys Info   │ │ (more)  │ │ │
│  │  │ (PTY)    │ │ (static)   │ │         │ │ │
│  │  └──────────┘ └────────────┘ └─────────┘ │ │
│  └──────────────────────────────────────────┘ │
└──────────────────────────────────────────────┘
```

### Process Lifetime

1. Map framebuffer via `syscall(513, &fb_info)`
2. Open `/dev/mouse` (O_RDONLY) and keyboard fd (stdin, fd 0)
3. Create epoll instance, register mouse fd + keyboard fd
4. Draw desktop background + taskbar
5. Open initial windows (terminal + system info)
6. Enter event loop: `epoll_wait(epfd, events, MAX, 16)` — 16ms timeout (~60fps cap)
7. On keyboard event: dispatch to focused window's key handler
8. On mouse event: update cursor position, check for compositor actions (drag, focus, close), dispatch to client area
9. On timeout (no events): redraw if dirty flag set
10. On quit signal or close-all: clear framebuffer, exit

### Rendering Strategy

Full backbuffer composite. Lumen maintains a backbuffer (`uint32_t *backbuf`, same size as framebuffer). All drawing goes to the backbuffer first. When the frame is complete, `memcpy` backbuffer → framebuffer. This eliminates flicker and tearing.

The cursor is drawn directly on the framebuffer AFTER the backbuffer copy, using save-under for the region underneath.

Dirty tracking: a single boolean `needs_redraw`. Set by any event that changes visual state (window move, focus change, window content update). The redraw path composites the entire backbuffer — no partial-rect optimization in v0.1.

---

## 2. Window System

### Window Struct

```c
#define MAX_WINDOWS     16
#define TITLEBAR_HEIGHT 30
#define BORDER_WIDTH    1

typedef struct window {
    int x, y;               /* top-left of window (including chrome) */
    int w, h;               /* client area dimensions (excluding chrome) */
    uint32_t *pixels;       /* client area pixel buffer (w * h * 4 bytes) */
    char title[64];
    int visible;
    int closeable;          /* 0 = no close button (e.g. desktop) */

    /* Callbacks */
    void (*on_key)(struct window *self, char key);
    void (*on_mouse)(struct window *self, int btn, int x, int y);
    void (*on_draw)(struct window *self);  /* called before composite */
    void *priv;              /* window-specific private data */
} window_t;
```

### Z-Order

A flat array `window_t *windows[MAX_WINDOWS]`. Index 0 = bottom, index `nwindows-1` = top. `raise_window(w)` removes `w` from its current position and appends it at the end.

### Focus

Click-to-focus. On mouse button down:
1. Find topmost visible window whose bounds contain (cursor_x, cursor_y), searching from top of z-order
2. If found and different from current focus, raise it and set as focused
3. Focused window gets a highlighted title bar; unfocused windows get a dimmed title bar

### Titlebar Drag

On mouse button down in a window's title bar region (y within [window.y, window.y + TITLEBAR_HEIGHT]):
1. Enter drag mode: record offset `(drag_dx, drag_dy)` = cursor - window origin
2. On mouse move while dragging: `window.x = cursor_x - drag_dx`, `window.y = cursor_y - drag_dy`, set `needs_redraw`
3. On mouse button up: exit drag mode

### Close Button

The close button is a small rectangle at the top-right of the title bar. On click, the window is removed from the window list and its pixel buffer freed. If it was focused, focus shifts to the next topmost window.

---

## 3. Window Chrome

Each window is drawn as:

1. **Drop shadow** — 4px offset, solid dark color (`#080810`, opaque — no alpha blending)
2. **Window body** — white rectangle (client area + title bar)
3. **Border** — 1px `#404060` around the entire window
4. **Title bar** — 30px gradient from `#3468A0` to `#205078` (same as fb_test), with text centered vertically
5. **Window buttons** — traffic-light style: red close (X), green maximize (ignored in v0.1), yellow minimize (ignored in v0.1)
6. **Client area** — blitted from `window->pixels`

Focused window gets a brighter title bar gradient. Unfocused gets a greyed-out title bar (`#606070` to `#404050`).

---

## 4. Software Cursor

### Sprite

A 16x16 monochrome arrow cursor, baked as a C array:
- 1-bit mask (which pixels are opaque)
- 1-bit color (black outline, white fill — standard arrow)
- Total: 16x16x2 bits = 64 bytes

### Save-Under

```c
uint32_t cursor_save[16 * 16];  /* saved pixels under cursor */
int cursor_saved_x, cursor_saved_y;
int cursor_visible;
```

Flow:
1. Before drawing cursor: save the 16x16 rect at (cursor_x, cursor_y) from the framebuffer into `cursor_save`
2. Draw cursor sprite onto framebuffer (mask-aware: only write opaque pixels)
3. On next mouse move: restore `cursor_save` to framebuffer at old position, then repeat from step 1 at new position

The cursor is drawn directly on the **framebuffer** (not the backbuffer). The backbuffer never has the cursor in it. This means:
- `composite()` draws everything to backbuf, then `memcpy` to FB
- After memcpy, save-under at cursor position, draw cursor on FB
- On cursor move: restore save-under, save-under at new pos, draw cursor

---

## 5. Input Handling

### Event Loop

**Note:** The kernel's `epoll` only supports socket fds (epoll_notify is only called from TCP/UDP). For v0.1, Lumen uses a **polling loop** with non-blocking reads and a short sleep to avoid busy-waiting.

All input fds are opened with `O_NONBLOCK`. The PTY master fd (for terminal output) is also non-blocking.

```c
int mouse_fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
/* stdin (fd 0) set to non-blocking + raw mode */
fcntl(0, F_SETFL, O_NONBLOCK);
struct termios raw;
tcgetattr(0, &raw);
raw.c_lflag &= ~(ECHO | ICANON | ISIG);  /* raw: no echo, no line buffering, no signals */
raw.c_cc[VMIN] = 0;
raw.c_cc[VTIME] = 0;
tcsetattr(0, TCSANOW, &raw);

for (;;) {
    int activity = 0;

    /* Poll keyboard */
    char key;
    if (read(0, &key, 1) == 1) {
        handle_keyboard(key);
        activity = 1;
    }

    /* Poll mouse */
    mouse_event_t mevt;
    while (read(mouse_fd, &mevt, sizeof(mevt)) == sizeof(mevt)) {
        handle_mouse(&mevt);
        activity = 1;
    }

    /* Poll PTY master (terminal output from shell) */
    if (term_window && term_window->priv) {
        char buf[256];
        int n = read(term_master_fd, buf, sizeof(buf));
        if (n > 0) {
            terminal_write(term_window, buf, n);
            activity = 1;
        }
    }

    if (needs_redraw) {
        composite();
        needs_redraw = 0;
    }

    /* Sleep ~16ms if idle to avoid burning CPU (~60fps when active) */
    if (!activity) {
        struct timespec ts = {0, 16000000};  /* 16ms */
        nanosleep(&ts, NULL);
    }
}
```

**Raw TTY mode is critical.** Without disabling `ISIG`, Ctrl-C kills Lumen instead of being forwarded to the terminal window's shell. Without disabling `ICANON`, input is line-buffered and single keystrokes don't arrive until Enter is pressed.

### Keyboard Dispatch

Read one byte from stdin. If a window is focused and has an `on_key` callback, call it with the ASCII character. Otherwise, handle compositor-level shortcuts:
- `Alt+F4` or similar: close focused window (deferred — requires keycode tracking, not just ASCII)
- No compositor shortcuts in v0.1 beyond what the focused window handles

### Mouse Dispatch

Read `mouse_event_t` from `/dev/mouse`. Update `cursor_x += dx`, `cursor_y += dy` (clamped to screen bounds). Then:

1. If dragging: update window position, set `needs_redraw`
2. If button pressed (transition from 0→1):
   - Check if click is on a window's close button → close window
   - Check if click is on a window's title bar → start drag, raise + focus
   - Check if click is on a window's client area → raise + focus, forward to window's `on_mouse`
   - Check if click is on taskbar → handle taskbar action
3. If button released: end drag
4. Always: update cursor on framebuffer (save-under dance)

---

## 6. Desktop & Taskbar

### Background

Vertical gradient from `#1B2838` to `#0D1B2A` (same as fb_test). Drawn once at startup into the backbuffer's lowest layer.

### Taskbar

36px bar at the bottom of the screen:
- Background: `#1A1A2E`
- Top border: 1px `#4488CC` accent line
- Left: "Aegis" button (90x24, blue background) — v0.1: no action
- Right: clock showing current time — v0.1: static "12:00 AM" (no RTC syscall)

---

## 7. Built-in Windows

### Terminal Window

The primary window. Uses a PTY pair:
- Lumen opens `/dev/ptmx` to get master fd + `/dev/pts/N` slave
- Forks a child process, sets slave as stdin/stdout/stderr, execs `/bin/oksh`
- Master fd is added to epoll — when shell outputs data, Lumen reads it and renders into the terminal window's pixel buffer
- Keyboard events in the terminal window are written to the master fd

The terminal has:
- Default size scales to display: `(fb_width * 3/5) / 10` columns × `(fb_height * 3/5 - TITLEBAR_HEIGHT) / 20` rows. On 1080p: ~115×51. On 1024×768: ~61×22.
- Green-on-black color scheme (`#40FF40` on `#0A0A14`)
- Basic character rendering (no ANSI escape sequence parsing in v0.1 — raw text)
- Cursor: blinking underscore or solid block (simple toggle every 500ms)
- Scrollback: none in v0.1 — oldest lines scroll off the top

### System Info Window

Static window displaying system information (same content as fb_test's info panel):
- Kernel version, architecture, display resolution, security model, shell, init
- Blue header "SYSTEM", separator line, info rows
- No interactivity — purely decorative

---

## 8. File Structure

| File | Responsibility |
|------|---------------|
| `user/lumen/main.c` | Entry point, event loop, initialization |
| `user/lumen/compositor.c` | Window list, z-order, composite(), background, chrome drawing |
| `user/lumen/compositor.h` | window_t struct, compositor API |
| `user/lumen/cursor.c` | Cursor sprite, save-under, draw/restore |
| `user/lumen/cursor.h` | Cursor API |
| `user/lumen/draw.c` | Drawing primitives (px, fill_rect, draw_rect, gradient, text) |
| `user/lumen/draw.h` | Drawing API |
| `user/lumen/terminal.c` | PTY terminal window implementation |
| `user/lumen/terminal.h` | Terminal API |
| `user/lumen/taskbar.c` | Taskbar rendering |
| `user/lumen/terminus20.h` | Font data (symlink or copy from fb_test) |
| `user/lumen/Makefile` | Build with musl-gcc, dynamically linked |

---

## 9. Testing

### test_lumen.py

Boot QEMU q35 with xHCI + USB mouse + USB keyboard. After login, run `lumen` (or it starts via vigil). Verify via serial output (lumen should print a startup message to serial before taking the framebuffer):

1. `[LUMEN] started WxH` appears in serial
2. Mouse injection via QEMU monitor causes cursor movement (verify via lumen's debug serial output or screenshot comparison)
3. Keyboard injection produces shell output in terminal window (verify via PTY master → serial echo)

This test is best-effort given the visual nature. The primary validation is: lumen starts without crashing and handles input without hanging.

### Boot Oracle

No changes to `tests/expected/boot.txt`. Lumen is a userspace binary, not a kernel subsystem.

---

## 10. Vigil Integration

Add a vigil service descriptor for lumen that starts it after login:

```
/etc/vigil/services/lumen/run: /bin/lumen
/etc/vigil/services/lumen/policy: oneshot
```

Or, simpler for v0.1: the user types `lumen` at the shell prompt. Vigil integration is deferred until lumen is stable.

For v0.1: **manual launch only.** User types `lumen` after logging in.

---

## 11. Kernel Dependencies

Existing infrastructure used:
- `sys_fb_map` (syscall 513) — framebuffer mapping
- `/dev/mouse` — mouse events (Phase 36, **untested** — lumen will be the first real test)
- PTY (`/dev/ptmx` + `/dev/pts/N`) — terminal I/O (Phase 32)
- Dynamic linking — musl libc.so (Phase 33)
- `fork`/`execve` — spawning shell in terminal window
- `O_NONBLOCK` + `read` — polling loop (no epoll on non-socket fds)

**Potential kernel work:** If `/dev/mouse` non-blocking read doesn't work (returns -EAGAIN when empty), the mouse VFS read function may need an `O_NONBLOCK` check. Currently `mouse_read_fn` in `initrd.c` always calls `mouse_read_blocking()` which sleeps. It needs to check the file's flags and return -EAGAIN if non-blocking and buffer is empty.

---

## Forward Constraints

1. **No multi-process clients.** Lumen v0.1 is a monolithic process. External apps cannot create windows. Phase 38 (Glyph) will need an IPC protocol (likely shared memory + Unix domain sockets).

2. **No ANSI escape parsing.** The terminal window renders raw text. Programs that emit ANSI codes (colors, cursor movement) will display garbage. A basic VT100 parser is Phase 38+ work.

3. **No window resize.** Windows are fixed-size. Resize requires client notification and buffer reallocation.

4. **Static clock.** No RTC syscall exists. Taskbar clock shows "12:00 AM" always.

5. **Full-frame composite.** Every redraw recomposites the entire backbuffer. Dirty-rect optimization deferred.

6. **printk conflicts.** Kernel printk still writes to the framebuffer. Once lumen is running, kernel output corrupts the display. Phase 38 should add a kernel flag to suppress FB writes when a compositor is active.

7. **No Alt+Tab or compositor shortcuts.** Keyboard is raw ASCII from stdin. Modifier state (Alt, Ctrl) is consumed by the TTY layer before reaching lumen. Window switching is mouse-only in v0.1.

8. **Mouse Phase 36 untested.** The USB HID mouse driver and /dev/mouse VFS device were implemented but not verified via `make test`. Lumen development will be the first real test of mouse input.
