# Lumen External Window Protocol — Design Spec

**Date:** 2026-04-15
**Status:** Approved
**Scope:** External process window support for Lumen compositor + installer port

---

## 1. Problem

The GUI installer (`/bin/gui-installer`) bypasses Lumen entirely: it calls `SYS_FB_MAP`
(syscall 513) to steal the physical framebuffer, renders directly, and has no window
chrome, z-ordering, or input routing through the compositor. This also requires the
`CAP_KIND_FB` capability, which a security-conscious installer should not need.

More broadly, Lumen has no way for a process outside its address space to register a
composited window. All windows today are `glyph_window_t` objects created inside Lumen's
own process (terminals, Citadel dock). This limits the GUI to whatever Lumen itself
spawns. Future apps (file manager, settings panel, etc.) need a general mechanism.

---

## 2. Goal

Design a minimal, versioned, binary protocol over AF_UNIX that lets any process open a
composited Lumen window. The window must:

- Appear with normal Lumen chrome (title bar, close button, shadow)
- Receive keyboard and mouse events from the compositor
- Be indistinguishable from internal windows to the rest of the compositor
- Persist correctly if the client process crashes (no zombie windows)

Port the GUI installer to use this protocol as the first client.

---

## 3. Architecture Overview

```
┌──────────────────────────────────────────────────┐
│  Lumen process                                   │
│                                                  │
│  lumen_server.c ──── /run/lumen.sock             │
│       │                    ↕  AF_UNIX            │
│  proxy_window_t  ←→  lumen_client.c              │
│       │                                          │
│  comp_add_window()                               │
│  compositor.c  (unchanged)                       │
└──────────────────────────────────────────────────┘
         ↑ memfd MAP_SHARED (shared pixel buffer)
┌─────────────────────┐
│  gui-installer      │
│  (external process) │
│  lumen_client.c     │
└─────────────────────┘
```

Lumen listens on `/run/lumen.sock`. The external process connects, negotiates a version
handshake, creates a window (receiving a memfd via `SCM_RIGHTS`), renders into a private
back buffer, and pushes frames by memcpy'ing to the shared memfd and sending a DAMAGE
notification. Lumen blits the shared buffer into the window's compositor surface on each
composite tick. Input events are serialized back over the same socket.

---

## 4. Wire Protocol

### 4.1 Common message header

All messages (both directions) begin with:

```c
typedef struct {
    uint32_t op;   /* opcode — see below */
    uint32_t len;  /* bytes of payload immediately following this header */
} lumen_msg_hdr_t;
```

Messages are read as: read 8-byte header, then read `len` bytes of payload. Zero-payload
messages have `len = 0`.

### 4.2 Handshake

On connect the client sends `lumen_hello_t`. Lumen replies with `lumen_hello_reply_t`.
If `status != 0` the client must close the socket.

```c
typedef struct {
    uint32_t magic;    /* 0x4C4D454E ("LMEN") */
    uint32_t version;  /* 1 */
} lumen_hello_t;

typedef struct {
    uint32_t magic;    /* 0x4C4D454E */
    uint32_t version;  /* echoed */
    uint32_t status;   /* 0 = OK, 1 = version unsupported */
} lumen_hello_reply_t;
```

These are sent as raw structs (no `lumen_msg_hdr_t` wrapper). They are the only messages
outside the header/payload framing.

### 4.3 Client → Lumen opcodes

| Opcode | Value | Payload struct |
|--------|-------|----------------|
| `LUMEN_OP_CREATE_WINDOW` | 1 | `lumen_create_window_t` |
| `LUMEN_OP_DAMAGE` | 2 | `lumen_damage_t` |
| `LUMEN_OP_SET_TITLE` | 3 | `lumen_set_title_t` |
| `LUMEN_OP_DESTROY_WINDOW` | 4 | `lumen_destroy_window_t` |

```c
typedef struct {
    uint16_t width;
    uint16_t height;
    char     title[64];  /* null-terminated, truncated if longer */
} lumen_create_window_t;

typedef struct {
    uint32_t window_id;
} lumen_damage_t;

typedef struct {
    uint32_t window_id;
    char     title[64];
} lumen_set_title_t;

typedef struct {
    uint32_t window_id;
} lumen_destroy_window_t;
```

### 4.4 `CREATE_WINDOW` reply

Lumen responds to `CREATE_WINDOW` via `sendmsg()` carrying both the reply struct and the
memfd as `SCM_RIGHTS`:

```c
typedef struct {
    uint32_t status;     /* 0 = OK, nonzero = error (errno) */
    uint32_t window_id;
    uint32_t width;      /* actual width assigned by Lumen */
    uint32_t height;     /* actual height assigned by Lumen */
} lumen_window_created_t;
```

The memfd is a file descriptor for a region of `width * height * 4` bytes (BGRA32).
The client maps it `MAP_SHARED | PROT_READ | PROT_WRITE`. Lumen maps it `MAP_SHARED |
PROT_READ`.

### 4.5 Lumen → client event opcodes

| Opcode | Value | Payload struct |
|--------|-------|----------------|
| `LUMEN_EV_KEY` | 0x10 | `lumen_key_event_t` |
| `LUMEN_EV_MOUSE` | 0x11 | `lumen_mouse_event_t` |
| `LUMEN_EV_CLOSE_REQUEST` | 0x12 | `lumen_close_request_t` |
| `LUMEN_EV_FOCUS` | 0x13 | `lumen_focus_event_t` |
| `LUMEN_EV_RESIZED` | 0x14 | `lumen_resized_event_t` |

```c
typedef struct {
    uint32_t window_id;
    uint32_t keycode;
    uint32_t modifiers;
    uint8_t  pressed;    /* 1 = key down, 0 = key up */
    uint8_t  _pad[3];
} lumen_key_event_t;

typedef struct {
    uint32_t window_id;
    int32_t  x, y;       /* client-area-relative coordinates */
    uint8_t  buttons;    /* bitmask: bit 0 = left, bit 1 = right, bit 2 = middle */
    uint8_t  evtype;     /* LUMEN_MOUSE_MOVE=0, DOWN=1, UP=2 */
    uint8_t  _pad[2];
} lumen_mouse_event_t;

typedef struct {
    uint32_t window_id;
} lumen_close_request_t;

typedef struct {
    uint32_t window_id;
    uint8_t  focused;    /* 1 = gained focus, 0 = lost */
    uint8_t  _pad[3];
} lumen_focus_event_t;

/* v1: not yet implemented — reserved for future resize support.
 * When implemented: Lumen sends this, then sends a new memfd via SCM_RIGHTS.
 * Client must unmap old memfd, map the new one, and re-render. */
typedef struct {
    uint32_t window_id;
    uint32_t new_width;
    uint32_t new_height;
} lumen_resized_event_t;
```

`LUMEN_EV_RESIZED` is defined in the protocol now so future clients can handle it without
a version bump, but Lumen v1 never sends it. Clients that receive an unknown opcode
(>= 0x14) must skip it (read and discard `len` bytes) rather than treating it as an error.

### 4.6 v1 known limitations

- Window size is fixed at creation time. Resize requires a future protocol extension
  (new memfd via `SCM_RIGHTS` inside a `LUMEN_EV_RESIZED` event).
- `LUMEN_OP_DAMAGE` damages the entire window surface. Dirty-rect damage
  (`LUMEN_OP_DAMAGE_RECT`) is reserved for a future version.
- Maximum 8 windows per client connection.

---

## 5. Client Library

### 5.1 New files

| File | Purpose |
|------|---------|
| `user/lib/glyph/lumen_proto.h` | Wire structs only. Included by both server and clients. |
| `user/lib/glyph/lumen_client.h` | Public client API. |
| `user/lib/glyph/lumen_client.c` | Client implementation (~250 LOC). |

Compiled into `libglyph.so` so all future Glyph apps get external window support
automatically.

### 5.2 Public API

```c
/* Connect to Lumen. Returns socket fd on success, -errno on failure.
 * Performs the version handshake before returning. */
int lumen_connect(void);

/* Create a window. Returns an opaque handle with a mapped back buffer.
 * win->backbuf is the private render target (malloc'd, w*h*4 bytes).
 * win->w, win->h, win->stride are set from the Lumen reply.
 * Returns NULL on failure. */
lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h);

/* Push a rendered frame:
 *   memcpy(win->shared, win->backbuf, win->stride * win->h)
 *   send DAMAGE(win->id)
 * Caller must have finished writing to backbuf before calling. */
void lumen_window_present(lumen_window_t *win);

/* Non-blocking event poll.
 * Returns 1 and fills *ev if an event is available, 0 if none.
 * Returns -1 on socket error (Lumen died or connection closed). */
int lumen_poll_event(int fd, lumen_event_t *ev);

/* Blocking event wait with optional timeout.
 * timeout_ms < 0: wait forever. timeout_ms = 0: equivalent to lumen_poll_event.
 * Returns 1 + fills *ev on event, 0 on timeout, -1 on error. */
int lumen_wait_event(int fd, lumen_event_t *ev, int timeout_ms);

/* Send DESTROY_WINDOW and unmap/free all resources. */
void lumen_window_destroy(lumen_window_t *win);
```

### 5.3 `lumen_window_t` struct

```c
typedef struct {
    int      fd;        /* connection socket */
    uint32_t id;        /* window_id from Lumen */
    int      memfd;     /* shared buffer fd */
    void    *shared;    /* MAP_SHARED — Lumen reads this */
    void    *backbuf;   /* malloc'd — client renders here */
    int      w, h, stride;
} lumen_window_t;
```

Client always renders into `backbuf`. `lumen_window_present()` owns the memcpy to
`shared`. This invariant makes partial-frame tearing impossible by construction.

### 5.4 `lumen_event_t`

```c
typedef struct {
    uint32_t type;       /* LUMEN_EV_* opcode */
    uint32_t window_id;
    union {
        struct { uint32_t keycode, modifiers; uint8_t pressed; } key;
        struct { int32_t x, y; uint8_t buttons, evtype; }        mouse;
        struct { uint8_t focused; }                               focus;
        struct { uint32_t new_w, new_h; }                        resized;
    };
} lumen_event_t;
```

---

## 6. Lumen Server Side

### 6.1 New files

| File | Purpose |
|------|---------|
| `user/bin/lumen/lumen_server.h` | Server interface (init, accept, poll). |
| `user/bin/lumen/lumen_server.c` | Server implementation (~300 LOC). |

### 6.2 `/run/` directory

Lumen calls `mkdir("/run", 0755)` during startup, ignoring `EEXIST`. This ensures
`/run/lumen.sock` can always be created regardless of rootfs state.

### 6.3 Per-client state

```c
#define LUMEN_MAX_WINDOWS_PER_CLIENT 8

typedef struct proxy_window proxy_window_t;

typedef struct {
    int              fd;
    proxy_window_t  *windows[LUMEN_MAX_WINDOWS_PER_CLIENT];
    int              nwindows;
    uint32_t         next_id;
} lumen_client_t;
```

### 6.4 Proxy window

```c
struct proxy_window {
    glyph_window_t  *win;     /* registered with compositor — normal window */
    lumen_client_t  *client;
    uint32_t         id;
    int              memfd;
    void            *shared;  /* MAP_SHARED — Lumen reads, client writes */
};
```

The proxy window's `on_render` callback blits from `shared` into the compositor surface:

```c
static void proxy_on_render(glyph_window_t *win, surface_t *surf) {
    proxy_window_t *pw = win->userdata;
    memcpy(surf->pixels, pw->shared, surf->w * surf->h * 4);
}
```

Chrome (title bar, close button, drop shadow) is drawn by `glyph_window_render()` as
normal — proxy windows are visually identical to internal windows.

### 6.5 `on_close` for proxy windows — important subtlety

The compositor's default `on_close` destroys the window immediately. For proxy windows
this must NOT happen. The proxy `on_close` handler instead sends `LUMEN_EV_CLOSE_REQUEST`
to the client over the socket and returns without destroying anything. The window is only
removed from the compositor when:

- The client sends `DESTROY_WINDOW`, **or**
- The client's socket closes (EOF/error) — see §6.6.

This allows clients to show confirmation dialogs before exiting.

### 6.6 Client death cleanup

When `read()` or `poll()` on a client fd returns EOF or error, `lumen_server_hangup()`
is called. It must:

1. For each `proxy_window_t` in `client->windows[]`:
   - `comp_remove_window(&compositor, pw->win)`
   - `munmap(pw->shared, pw->win->client_w * pw->win->client_h * 4)`
   - `close(pw->memfd)`
   - `glyph_window_free(pw->win)`
   - `free(pw)`
2. `close(client->fd)`
3. Remove client fd from Lumen's poll set
4. `free(client)`

Failure to do this leaves zombie windows on screen permanently.

### 6.7 Event loop integration

`lumen_server_init()` is called from `main.c` once during Lumen startup. It creates the
socket and adds the listen fd to Lumen's existing `poll()` fd set.

On each poll tick:
- Listen fd readable → `lumen_server_accept()`: `accept()`, perform handshake, allocate
  `lumen_client_t`, add client fd to poll set.
- Client fd readable → `lumen_server_read(client)`: read header + payload, dispatch on
  opcode. On EOF/error → `lumen_server_hangup(client)`.

No changes to `compositor.c` or the rendering path. Proxy windows are registered via the
existing `comp_add_window()` and are thereafter invisible to the compositor internals.

---

## 7. GUI Installer Port

### 7.1 What changes

| Current | New |
|---------|-----|
| `syscall(513, &fb_info)` — steals framebuffer | `lumen_connect()` + `lumen_window_create()` |
| Reads `/dev/kbd` directly (blocking) | `lumen_wait_event()` for input |
| `blit()` memcpy's to raw framebuffer | `lumen_window_present()` |
| Requires `CAP_KIND_FB` | No framebuffer capability needed |

### 7.2 Main loop shape

```c
int lfd = lumen_connect();
lumen_window_t *win = lumen_window_create(lfd, "Install Aegis", 800, 600);

while (!done) {
    render_current_screen(win->backbuf, win->w, win->h, win->stride);
    lumen_window_present(win);

    lumen_event_t ev;
    /* Block until input — installer has nothing to do between interactions */
    while (lumen_wait_event(lfd, &ev, -1) == 1) {
        if (ev.type == LUMEN_EV_CLOSE_REQUEST) { done = 1; break; }
        if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
            installer_handle_key(&state, ev.key.keycode, ev.key.modifiers);
        if (ev.type == LUMEN_EV_MOUSE)
            installer_handle_mouse(&state, ev.mouse.x, ev.mouse.y,
                                   ev.mouse.buttons, ev.mouse.evtype);
        if (state.needs_redraw) break;  /* re-render */
    }
}

lumen_window_destroy(win);
close(lfd);
```

The five installer screen rendering functions are unchanged. Only the framebuffer setup,
input source, and blit path change.

### 7.3 Capability change

Remove `CAP_KIND_FB` from `/etc/aegis/caps.d/gui-installer`. The installer no longer
maps the framebuffer at all. The socket path `/run/lumen.sock` is accessible to any
process with `CAP_KIND_VFS_OPEN` (baseline), so no new capability is needed.

### 7.4 Launch path

Citadel's desktop icon continues to `sys_spawn("/bin/gui-installer", ...)`. No changes
required to Citadel, Vigil, or the launch path.

---

## 8. Files Changed

| File | Change |
|------|--------|
| `user/lib/glyph/lumen_proto.h` | **New** — wire protocol structs |
| `user/lib/glyph/lumen_client.h` | **New** — client API |
| `user/lib/glyph/lumen_client.c` | **New** — client implementation |
| `user/lib/glyph/Makefile` | Add `lumen_client.o` |
| `user/bin/lumen/lumen_server.h` | **New** — server interface |
| `user/bin/lumen/lumen_server.c` | **New** — server implementation |
| `user/bin/lumen/main.c` | Call `lumen_server_init()`, add fds to poll set |
| `user/bin/lumen/compositor.c` | `comp_remove_window()` (if not already present) |
| `user/bin/gui-installer/main.c` | Port to `lumen_client` API |
| `rootfs/etc/aegis/caps.d/gui-installer` | Remove `CAP_KIND_FB` |

---

## 9. Testing

1. Boot q35 with GUI. Launch installer from desktop icon.
2. Verify installer appears as a normal Lumen window with title bar and close button.
3. Verify installer is composited behind other windows (open a terminal, drag it over).
4. Verify keyboard input works (type in text fields on installer screens).
5. Verify mouse input works (click buttons, navigate screens).
6. Verify close button sends `CLOSE_REQUEST` and installer exits cleanly.
7. Kill installer process with `kill -9` from another terminal. Verify no zombie window
   remains on screen (client death cleanup working).
8. Launch installer, complete an install, verify no resource leaks on clean exit.

---

## 10. Future Work (out of scope for this implementation)

- **Window resize** — requires new memfd via `SCM_RIGHTS` in `LUMEN_EV_RESIZED`. Protocol
  opcode reserved.
- **Dirty-rect damage** — `LUMEN_OP_DAMAGE_RECT(window_id, x, y, w, h)` for incremental
  redraws.
- **Capability gating** — connecting to `/run/lumen.sock` could require a future
  `CAP_KIND_GUI` capability rather than relying on filesystem permissions.
- **Multi-window per process** — already supported in the protocol (window_id) and server
  state (`windows[8]`), but untested in v1.
- **Window positioning hints** — `CREATE_WINDOW` could accept optional `x, y` to place
  the window at a specific location rather than Lumen's default centering.
