# Phase 39: Glyph Widget Toolkit + Lumen Optimization

## Goal

Build `libglyph.a`, a retained-mode widget toolkit for Aegis GUI apps, and optimize Lumen's compositor with dirty-rect tracking so the GUI is responsive on real hardware. All apps are compiled into Lumen for v0.1 (external app IPC is Phase 42).

## Architecture

Glyph is a static library (`libglyph.a`) that Lumen links. It provides a retained widget tree: apps create widgets once, register callbacks, and Glyph handles layout, rendering, hit-testing, and dirty-rect tracking. The existing `draw.c` primitives and Terminus font move into Glyph as the rendering foundation.

The compositor no longer knows about window chrome — Glyph windows render their own title bars, close buttons, and client areas. The compositor manages z-order, focus, and drag, then blits each window's pixel buffer during composite.

## Widget Catalog

### Base Widget (`glyph_widget_t`)

Embedded as the first member of every widget struct (C "inheritance"). Fields:

- `type` — enum identifying the widget kind
- `parent` — pointer to parent widget (NULL for root)
- `children[]`, `nchildren` — child widget array
- `x, y, w, h` — bounds relative to parent
- `pref_w, pref_h` — preferred size (used by layout)
- `dirty` — flag, set when state changes
- `visible` — flag
- `focusable` — flag (text field, list view: yes; label, image: no)
- `on_click(widget, btn, x, y)` — mouse click callback
- `on_key(widget, key)` — keyboard input callback
- `draw(widget, surface, clip_rect)` — render callback (each widget type sets this)

### Widgets

| Widget | Key State | User Callbacks | Rendering |
|--------|-----------|----------------|-----------|
| **Label** | text, color, align (left/center/right) | — | `draw_text` or `draw_text_t` |
| **Button** | text, state (normal/hover/pressed/disabled) | `on_click` | Raised rect + text; pressed = sunken; disabled = grayed |
| **Text field** | buf[256], cursor_pos, selection | `on_change(field, text)` | Bordered rect, text, blinking cursor. Handles printable, backspace, arrows. |
| **Checkbox** | checked (0/1), label text | `on_change(cb, checked)` | 16x16 box (checkmark when checked) + label |
| **List view** | items[] (string array), count, selected_idx, scroll_offset, visible_rows | `on_select(lv, idx)` | Rows with highlight on selected. Scrollable via arrow keys or attached scroll bar. |
| **Scroll bar** | min, max, value, page_size | `on_scroll(sb, value)` | Vertical track + draggable thumb. Click-in-track jumps by page_size. |
| **Image** | pixels (uint32_t *), w, h | — | Raw blit. No decoding — caller provides 32-bit ARGB pixel buffer. |
| **Progress bar** | value (0-100) | — | Bordered rect with filled portion proportional to value. |
| **Menu bar** | menus[] (each: label + item strings), open_idx (-1 = closed) | `on_menu_select(mb, menu_idx, item_idx)` | Horizontal labels at window top. Click opens dropdown overlay. Click item or Esc closes. |
| **Tabs** | labels[], active_idx, panels[] (each a glyph_box_t container) | `on_tab_change(tabs, idx)` | Tab header row; only active panel's children are visible/rendered. |
| **HBox / VBox** | direction (H/V), padding, spacing | — | Layout container. Computes child bounds: sequential placement with spacing. Children sized by pref_w/pref_h. |

### Window (`glyph_window_t`)

Top-level container. Replaces the current `window_t` in compositor.c.

- Owns a `surface_t` pixel buffer (the window's content including chrome)
- Has a root widget (typically a VBox holding menu bar + client content)
- Renders window chrome: title bar with gradient, close button (X), title text
- Tracks dirty rect (union of all dirty widget bounds)
- Provides `glyph_window_render(win)` — walks tree, redraws dirty widgets only
- Provides `glyph_window_dispatch_mouse(win, btn, x, y)` — hit-test, dispatch to deepest widget
- Provides `glyph_window_dispatch_key(win, key)` — dispatch to focused widget
- Provides `glyph_window_get_dirty_rect(win, rect_out)` — returns dirty region for compositor

Client area starts below the title bar. Widget coordinates are relative to client area origin.

### Focus

- One widget per window holds keyboard focus.
- Tab key cycles focus among `focusable` widgets (depth-first order).
- Click on a focusable widget gives it focus.
- Focused widget gets a visual focus ring (2px border).
- `glyph_window_set_focus(win, widget)` for programmatic focus.

### Layout

HBox and VBox are the only layout containers. They arrange children sequentially:

- **HBox**: left-to-right, children at `x = prev_x + prev_w + spacing`
- **VBox**: top-to-bottom, children at `y = prev_y + prev_h + spacing`
- `padding` — space between container edge and first/last child
- Children use their `pref_w`/`pref_h` for sizing
- No stretch/flex for v0.1 — fixed preferred sizes only

This is deliberately simple. A constraint solver is unnecessary for the widgets we have.

## Lumen Optimizations

### Dirty-Rect Pipeline

1. **Input arrives** (key, mouse event, PTY bytes)
2. **Glyph state changes** → widget sets `dirty = 1` → `glyph_window_t` unions widget bounds into its dirty rect
3. **Main loop** polls all inputs, then checks each window for dirty rects. Also adds old + new cursor rects if cursor moved.
4. **Union all dirty rects** into a single bounding rect (or a small list of rects — single union is simpler for v0.1).
5. **If empty → skip frame entirely** (skip-if-clean).
6. **Render**: call `glyph_window_render()` only for windows with dirty content. Only widgets intersecting the dirty rect are redrawn.
7. **Composite**: blit only dirty regions from window surfaces → backbuffer.
8. **FB flip**: copy only dirty regions from backbuffer → framebuffer (partial WC write).
9. **Cursor**: save-under + draw in 16x16 rect only.

### Mouse Batching

Drain all pending `/dev/mouse` events per frame into accumulated (dx, dy, buttons). Process as a single movement. One composite per frame, not per event.

### Terminal Batching

Drain all available bytes from PTY master fd before marking terminal dirty. One render per frame, not per byte.

### Window Content Caching

`glyph_window_render()` is only called if the window's dirty rect is non-empty. Otherwise the existing pixel buffer is reused during composite — just blit the cached surface.

### Partial FB Blit

Replace `memcpy(fb, back, total_size)` with per-dirty-rect row copies:

```c
for each dirty_rect:
    for row in rect.y .. rect.y + rect.h:
        memcpy(fb + row*pitch + rect.x, back + row*pitch + rect.x, rect.w * 4)
```

This is the critical optimization. Mouse movement goes from ~8MB WC writes to ~4KB.

### Mouse Rate Limiting

Already handled by mouse batching — all events per frame collapsed into one delta.

## File Structure

```
user/glyph/
  glyph.h          — public API: all widget types, create/destroy, window, layout
  widget.c          — base widget tree operations: add/remove child, dirty propagation,
                      hit-test walk, focus cycling, generic dispatch
  window.c          — glyph_window_t: chrome rendering, dirty rect tracking,
                      render walk, mouse/key dispatch to widget tree
  label.c           — label widget
  button.c          — button widget
  textfield.c       — text field widget
  checkbox.c        — checkbox widget
  listview.c        — list view widget
  scrollbar.c       — scroll bar widget
  image.c           — image widget
  progress.c        — progress bar widget
  menubar.c         — menu bar + dropdown menus
  tabs.c            — tabbed container widget
  box.c             — hbox/vbox layout containers
  draw.c            — rendering primitives (moved from user/lumen/)
  draw.h            — draw API header
  terminus20.h      — Terminus 10x20 font data (moved from user/lumen/)
  Makefile          — builds libglyph.a

user/lumen/
  main.c            — event loop: mouse batching, PTY batching, skip-if-clean, frame pacing
  compositor.c      — dirty-rect composite, partial FB blit, glyph_window_t integration
  compositor.h      — compositor API
  cursor.c          — dirty-rect-aware save-under cursor
  cursor.h          — cursor API
  terminal.c        — PTY terminal as a glyph_window with text grid widget
  terminal.h        — terminal API
  taskbar.c         — taskbar as a glyph_window with label + button widgets
  Makefile          — links libglyph.a via -L../glyph -lglyph -I../glyph
```

`draw.c`, `draw.h`, and `terminus20.h` move from `user/lumen/` to `user/glyph/`. `user/fb_test/` can also use `-I../glyph` to eliminate its own copy.

## Compositor Changes

The `window_t` struct is replaced by `glyph_window_t *` in the compositor's window array. The compositor's responsibilities narrow to:

- **Z-order management** — array of `glyph_window_t *`, raise/lower
- **Window drag** — title bar hit-test delegated to `glyph_window_t`, compositor handles position updates
- **Focus tracking** — which window is active
- **Dirty-rect union** — collect dirty rects from all windows + cursor, compute composite region
- **Partial composite** — blit dirty regions from window surfaces to backbuffer
- **Partial FB flip** — copy dirty regions from backbuffer to framebuffer
- **Cursor** — save-under + draw at cursor position

The compositor no longer draws window chrome, title bars, close buttons, or gradients. All of that is inside `glyph_window_t`.

## Terminal Integration

The terminal stays as a specialized window. Its content area uses a character grid (not Glyph widgets — a text grid with custom rendering is more appropriate than 5000 label widgets). But it is wrapped in a `glyph_window_t` for chrome and dirty-rect integration:

- `glyph_window_t` provides title bar and chrome
- Client area rendering is custom: `term_render()` draws the text grid into the window's surface
- PTY bytes → `terminal_write()` → marks the window dirty
- Keyboard → `glyph_window_dispatch_key()` → terminal's key handler → write to PTY master

## Keyboard Forwarding Fix

On bare metal, keyboard input wasn't reaching the terminal. The issue is likely that Lumen reads stdin in raw mode (`VMIN=0, VTIME=0`) but the keyboard path through the IOAPIC-routed IRQ may behave differently than PIC-routed. Debug steps:

1. Verify stdin reads return bytes when keys are pressed (add a debug indicator — e.g., flash a pixel on keypress)
2. If bytes arrive but don't reach the terminal, trace the dispatch path: `main.c read()` → `comp_handle_key()` → focused window → `on_key()` → PTY master write
3. If no bytes arrive, check that IRQ1 (keyboard) is properly routed through IOAPIC and the kbd VFS path works

This may be a one-line fix or may need IOAPIC routing investigation. Investigate during implementation.

## Testing

No automated QEMU pixel test (fragile). Manual verification:

1. Boot ISO on ThinkPad X13
2. Launch Lumen from shell
3. Verify: terminal window renders with Glyph chrome
4. Verify: keyboard input reaches terminal (type commands, see output)
5. Verify: mouse movement is responsive (should be dramatically faster than Phase 37)
6. Verify: window drag works
7. Verify: taskbar renders with Glyph widgets

## Phase 42 IPC Requirements (Forward Note)

For external Glyph apps (not compiled into Lumen) to create windows, Phase 42 needs:

- **MAP_SHARED** — shared memory regions between processes for pixel buffers
- **Unix domain sockets or named pipes** — command channel for window create/destroy/resize/focus
- **fd passing** — compositor passes shared memory fd to client
- A simple display server protocol: client sends (create_window, set_title, present_buffer, close), compositor sends (key, mouse, resize, focus) events

Until Phase 42, all GUI apps are window types compiled into Lumen.

## Dependencies

- Phase 37 (Lumen) — base compositor, must be complete
- Phase 38 (SMP) — IOAPIC routing for keyboard on bare metal
- `musl-gcc` dynamic toolchain — for building Lumen
- `CAP_KIND_FB` — framebuffer capability (added in Phase 38 testing)
