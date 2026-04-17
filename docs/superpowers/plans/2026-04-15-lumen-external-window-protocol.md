# Lumen External Window Protocol — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an AF_UNIX socket protocol to Lumen so external processes can open composited windows, then port the GUI installer to use it.

**Architecture:** Lumen opens `/run/lumen.sock`. Clients connect, negotiate a version handshake, create windows that receive a shared memfd pixel buffer, render into a private backbuf, and push frames by memcpy + DAMAGE notification. Lumen blits the shared buffer into the window's compositor surface via an `on_render` proxy callback. Input events route back over the same socket. Chrome (title bar, close button) is drawn by glyph_window_render as normal — proxy windows are visually identical to internal windows.

**Tech Stack:** C (musl), AF_UNIX SOCK_STREAM, `sendmsg`/`recvmsg` with SCM_RIGHTS for memfd passing, `memfd_create` + `MAP_SHARED`, `libglyph.a`, existing `compositor_t` + `comp_add_window` / `comp_remove_window` API.

---

## File Map

| File | Change |
|------|--------|
| `user/lib/glyph/lumen_proto.h` | **New** — wire structs (both client and server include this) |
| `user/lib/glyph/lumen_client.h` | **New** — public client API |
| `user/lib/glyph/lumen_client.c` | **New** — client implementation (~250 LOC) |
| `user/lib/glyph/Makefile` | Add `lumen_client.c` to SRCS |
| `user/bin/lumen/lumen_server.h` | **New** — server interface |
| `user/bin/lumen/lumen_server.c` | **New** — server implementation (~300 LOC) |
| `user/bin/lumen/main.c` | Call `lumen_server_init()` at startup; add `lumen_server_tick()` in event loop |
| `user/bin/lumen/Makefile` | Add `lumen_server.c` to SRCS |
| `user/bin/gui-installer/main.c` | Port to `lumen_client` API |
| `rootfs/etc/aegis/caps.d/gui-installer` | Remove `FB` cap |

---

## Task 1: Wire protocol header + skeleton files + Makefile wiring

**Files:**
- Create: `user/lib/glyph/lumen_proto.h`
- Create: `user/lib/glyph/lumen_client.h`
- Create: `user/lib/glyph/lumen_client.c`
- Modify: `user/lib/glyph/Makefile`
- Create: `user/bin/lumen/lumen_server.h`
- Create: `user/bin/lumen/lumen_server.c`
- Modify: `user/bin/lumen/Makefile`

- [ ] **Step 1: Create `user/lib/glyph/lumen_proto.h`**

```c
/* lumen_proto.h — Lumen external window protocol wire structs.
 * Included by both the client library and the Lumen server. */
#ifndef LUMEN_PROTO_H
#define LUMEN_PROTO_H

#include <stdint.h>

#define LUMEN_MAGIC     0x4C4D454Eu  /* "LMEN" */
#define LUMEN_VERSION   1u

/* ── Handshake (no lumen_msg_hdr_t wrapper — sent raw) ──────────────── */

typedef struct {
    uint32_t magic;    /* LUMEN_MAGIC */
    uint32_t version;  /* LUMEN_VERSION */
} lumen_hello_t;

typedef struct {
    uint32_t magic;    /* LUMEN_MAGIC */
    uint32_t version;  /* echoed */
    uint32_t status;   /* 0=OK, 1=version unsupported, 2=server full */
} lumen_hello_reply_t;

/* ── Common framed message header ───────────────────────────────────── */

typedef struct {
    uint32_t op;   /* opcode */
    uint32_t len;  /* bytes of payload following this header */
} lumen_msg_hdr_t;

/* ── Client → server opcodes ────────────────────────────────────────── */

#define LUMEN_OP_CREATE_WINDOW   1u
#define LUMEN_OP_DAMAGE          2u
#define LUMEN_OP_SET_TITLE       3u
#define LUMEN_OP_DESTROY_WINDOW  4u

typedef struct {
    uint16_t width;
    uint16_t height;
    char     title[64];  /* null-terminated */
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

/* ── Server → client (CREATE_WINDOW reply) ──────────────────────────── */
/* Sent as framed message (hdr.op=0) via sendmsg with SCM_RIGHTS memfd  */

typedef struct {
    uint32_t status;     /* 0=OK, nonzero=errno */
    uint32_t window_id;
    uint32_t width;      /* actual width Lumen assigned */
    uint32_t height;     /* actual height Lumen assigned */
} lumen_window_created_t;

/* ── Server → client event opcodes ─────────────────────────────────── */

#define LUMEN_EV_KEY           0x10u
#define LUMEN_EV_MOUSE         0x11u
#define LUMEN_EV_CLOSE_REQUEST 0x12u
#define LUMEN_EV_FOCUS         0x13u
#define LUMEN_EV_RESIZED       0x14u  /* v1: defined but never sent */

/* LUMEN_EV_MOUSE evtype values */
#define LUMEN_MOUSE_MOVE  0u
#define LUMEN_MOUSE_DOWN  1u
#define LUMEN_MOUSE_UP    2u

typedef struct {
    uint32_t window_id;
    uint32_t keycode;    /* (uint8_t)char from keyboard ISR */
    uint32_t modifiers;  /* reserved, 0 in v1 */
    uint8_t  pressed;    /* 1=down */
    uint8_t  _pad[3];
} lumen_key_event_t;

typedef struct {
    uint32_t window_id;
    int32_t  x, y;       /* client-area-relative (after subtracting chrome) */
    uint8_t  buttons;    /* bitmask: bit0=left, bit1=right, bit2=middle */
    uint8_t  evtype;     /* LUMEN_MOUSE_* */
    uint8_t  _pad[2];
} lumen_mouse_event_t;

typedef struct {
    uint32_t window_id;
} lumen_close_request_t;

typedef struct {
    uint32_t window_id;
    uint8_t  focused;
    uint8_t  _pad[3];
} lumen_focus_event_t;

typedef struct {
    uint32_t window_id;
    uint32_t new_width;
    uint32_t new_height;
} lumen_resized_event_t;

#endif /* LUMEN_PROTO_H */
```

- [ ] **Step 2: Create `user/lib/glyph/lumen_client.h`**

```c
/* lumen_client.h — Client API for connecting external processes to Lumen */
#ifndef LUMEN_CLIENT_H
#define LUMEN_CLIENT_H

#include <stdint.h>
#include "lumen_proto.h"

/* Opaque window handle */
typedef struct {
    int      fd;      /* connection socket */
    uint32_t id;      /* window_id assigned by Lumen */
    int      memfd;   /* shared pixel buffer fd */
    void    *shared;  /* MAP_SHARED — Lumen reads this */
    void    *backbuf; /* malloc'd — caller renders here */
    int      w, h;    /* client area dimensions in pixels */
    int      stride;  /* stride in pixels (== w in v1) */
} lumen_window_t;

/* Unified event struct */
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

/* Connect to Lumen and perform version handshake.
 * Returns socket fd on success, -errno on failure. */
int lumen_connect(void);

/* Create a composited window.
 * win->backbuf is the private render target (malloc'd, w*h*4 bytes).
 * Returns NULL on failure. */
lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h);

/* Push a frame: memcpy(shared, backbuf, w*h*4), then send DAMAGE.
 * Call after finishing a render into backbuf. */
void lumen_window_present(lumen_window_t *win);

/* Non-blocking event poll.
 * Returns 1+fills *ev, 0 if no event, -1 on socket error. */
int lumen_poll_event(int fd, lumen_event_t *ev);

/* Blocking event wait with optional timeout.
 * timeout_ms < 0: wait forever. timeout_ms >= 0: timeout in ms.
 * Returns 1+fills *ev on event, 0 on timeout, -1 on error. */
int lumen_wait_event(int fd, lumen_event_t *ev, int timeout_ms);

/* Send DESTROY_WINDOW and free all resources. */
void lumen_window_destroy(lumen_window_t *win);

#endif /* LUMEN_CLIENT_H */
```

- [ ] **Step 3: Create `user/lib/glyph/lumen_client.c` (skeleton — just stubs)**

```c
/* lumen_client.c — Lumen external window protocol client implementation */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "lumen_client.h"

int lumen_connect(void) { return -ENOSYS; }

lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h)
{
    (void)fd; (void)title; (void)w; (void)h;
    return NULL;
}

void lumen_window_present(lumen_window_t *win) { (void)win; }
int  lumen_poll_event(int fd, lumen_event_t *ev) { (void)fd; (void)ev; return 0; }
int  lumen_wait_event(int fd, lumen_event_t *ev, int t) { (void)fd; (void)ev; (void)t; return 0; }
void lumen_window_destroy(lumen_window_t *win) { (void)win; }
```

- [ ] **Step 4: Create `user/bin/lumen/lumen_server.h`**

```c
/* lumen_server.h — Lumen external window server interface */
#ifndef LUMEN_SERVER_H
#define LUMEN_SERVER_H

#include "compositor.h"

/* Create /run/lumen.sock and start listening.
 * Returns listen fd on success, -1 on failure. */
int lumen_server_init(void);

/* Non-blocking poll: accept new connections, read pending client messages.
 * Call once per event loop iteration.
 * Returns 1 if any compositor window was dirtied, 0 otherwise. */
int lumen_server_tick(compositor_t *comp, int listen_fd);

#endif /* LUMEN_SERVER_H */
```

- [ ] **Step 5: Create `user/bin/lumen/lumen_server.c` (skeleton)**

```c
/* lumen_server.c — Lumen external window protocol server */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <glyph.h>
#include "compositor.h"
#include "lumen_server.h"
#include "lumen_proto.h"

int lumen_server_init(void) { return -1; }

int lumen_server_tick(compositor_t *comp, int listen_fd)
{
    (void)comp; (void)listen_fd;
    return 0;
}
```

- [ ] **Step 6: Update `user/lib/glyph/Makefile` — add `lumen_client.c`**

Change:
```makefile
SRCS = draw.c widget.c window.c box.c label.c button.c textfield.c \
       checkbox.c progress.c image.c scrollbar.c listview.c menubar.c tabs.c \
       font.c
```
To:
```makefile
SRCS = draw.c widget.c window.c box.c label.c button.c textfield.c \
       checkbox.c progress.c image.c scrollbar.c listview.c menubar.c tabs.c \
       font.c lumen_client.c
```

- [ ] **Step 7: Update `user/bin/lumen/Makefile` — add `lumen_server.c`**

Change:
```makefile
SRCS = main.c cursor.c compositor.c terminal.c widget_test.c about.c
```
To:
```makefile
SRCS = main.c cursor.c compositor.c terminal.c widget_test.c about.c \
       lumen_server.c
```

- [ ] **Step 8: Verify skeleton compiles (on x86 build box via SSH)**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 "cd ~/Developer/aegis && make -C user/lib/glyph 2>&1 | tail -5"
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 "cd ~/Developer/aegis && make -C user/bin/lumen 2>&1 | tail -5"
```
Expected: both compile with no errors.

- [ ] **Step 9: Commit**

```bash
git add user/lib/glyph/lumen_proto.h user/lib/glyph/lumen_client.h \
        user/lib/glyph/lumen_client.c user/lib/glyph/Makefile \
        user/bin/lumen/lumen_server.h user/bin/lumen/lumen_server.c \
        user/bin/lumen/Makefile
git commit -m "lumen: add wire protocol header + client/server skeleton files"
```

---

## Task 2: Server socket init + handshake + `lumen_connect` + Lumen main.c integration

**Files:**
- Modify: `user/lib/glyph/lumen_client.c` — implement `lumen_connect`
- Modify: `user/bin/lumen/lumen_server.c` — implement `lumen_server_init`, `lumen_server_accept`
- Modify: `user/bin/lumen/main.c` — call `lumen_server_init()`, call `lumen_server_tick()` in loop

- [ ] **Step 1: Implement `lumen_server_init` in `lumen_server.c`**

Add after the `#include` block — module state:
```c
#define LUMEN_MAX_CLIENTS            8
#define LUMEN_MAX_WINDOWS_PER_CLIENT 8

/* Forward declaration */
typedef struct proxy_window proxy_window_t;

typedef struct {
    int             fd;
    proxy_window_t *windows[LUMEN_MAX_WINDOWS_PER_CLIENT];
    int             nwindows;
    uint32_t        next_id;
} lumen_client_t;

struct proxy_window {
    glyph_window_t *win;
    lumen_client_t *client;
    uint32_t        id;
    int             memfd;
    void           *shared;   /* MAP_SHARED PROT_READ — Lumen reads this */
};

static lumen_client_t *s_clients[LUMEN_MAX_CLIENTS];
static int              s_ncli;
```

Replace the `lumen_server_init` stub:
```c
int lumen_server_init(void)
{
    /* Ensure /run exists */
    mkdir("/run", 0755);  /* ignore EEXIST */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* Remove stale socket file */
    unlink("/run/lumen.sock");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/lumen.sock",
            sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }

    /* Non-blocking so tick() accept never blocks */
    fcntl(fd, F_SETFL, O_NONBLOCK);

    return fd;
}
```

- [ ] **Step 2: Implement `lumen_server_accept` (static helper in `lumen_server.c`)**

Add after the module state block (before `lumen_server_init`):
```c
static void lumen_server_accept(compositor_t *comp)
{
    (void)comp;

    int fd = accept(s_listen_fd_unused, NULL, NULL);  /* placeholder — fix in step 3 */
    if (fd < 0) return;

    /* Set non-blocking immediately */
    fcntl(fd, F_SETFL, O_NONBLOCK);

    /* Wait up to 500ms for hello (client sends immediately after connect) */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) <= 0) { close(fd); return; }

    lumen_hello_t hello;
    if (read(fd, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        close(fd);
        return;
    }

    lumen_hello_reply_t reply;
    reply.magic   = LUMEN_MAGIC;
    reply.version = LUMEN_VERSION;

    if (hello.magic != LUMEN_MAGIC || hello.version != LUMEN_VERSION) {
        reply.status = 1;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    if (s_ncli >= LUMEN_MAX_CLIENTS) {
        reply.status = 2;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    reply.status = 0;
    write(fd, &reply, sizeof(reply));

    lumen_client_t *cli = calloc(1, sizeof(*cli));
    if (!cli) { close(fd); return; }
    cli->fd      = fd;
    cli->next_id = 1;
    s_clients[s_ncli++] = cli;
}
```

Note: `s_listen_fd_unused` is a placeholder — in Step 3 we fix `lumen_server_tick` so `accept()` is called there instead, removing the need to store the fd twice.

- [ ] **Step 3: Fix `lumen_server_tick` to call accept properly**

Replace the `lumen_server_tick` stub with this real structure (window dispatch will be added later):

```c
int lumen_server_tick(compositor_t *comp, int listen_fd)
{
    int dirtied = 0;

    /* Non-blocking accept */
    {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0)
            lumen_server_accept_fd(comp, listen_fd);
    }

    /* Read from each connected client (forward, handle removal via continue) */
    for (int i = 0; i < s_ncli; ) {
        struct pollfd pfd = { .fd = s_clients[i]->fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            int r = lumen_server_read(comp, s_clients[i]);
            if (r < 0) {
                lumen_server_hangup(comp, s_clients[i]);
                /* s_clients[i] now contains next client (swap-with-last) */
                continue;
            }
            if (r > 0)
                dirtied = 1;
        }
        i++;
    }

    return dirtied;
}
```

And rename `lumen_server_accept` to `lumen_server_accept_fd`, removing the `s_listen_fd_unused` reference:
```c
static void lumen_server_accept_fd(compositor_t *comp, int listen_fd)
{
    (void)comp;

    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;

    fcntl(fd, F_SETFL, O_NONBLOCK);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) <= 0) { close(fd); return; }

    lumen_hello_t hello;
    if (read(fd, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        close(fd);
        return;
    }

    lumen_hello_reply_t reply;
    reply.magic   = LUMEN_MAGIC;
    reply.version = LUMEN_VERSION;

    if (hello.magic != LUMEN_MAGIC || hello.version != LUMEN_VERSION) {
        reply.status = 1;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    if (s_ncli >= LUMEN_MAX_CLIENTS) {
        reply.status = 2;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    reply.status = 0;
    write(fd, &reply, sizeof(reply));

    lumen_client_t *cli = calloc(1, sizeof(*cli));
    if (!cli) { close(fd); return; }
    cli->fd      = fd;
    cli->next_id = 1;
    s_clients[s_ncli++] = cli;
}
```

Also add stub functions that `lumen_server_tick` calls (so it compiles):
```c
static int  lumen_server_read(compositor_t *comp, lumen_client_t *cli)
    { (void)comp; (void)cli; return 0; }
static void lumen_server_hangup(compositor_t *comp, lumen_client_t *cli)
    { (void)comp; close(cli->fd); free(cli); }
```

- [ ] **Step 4: Implement `lumen_connect` in `lumen_client.c`**

Replace the `lumen_connect` stub:
```c
int lumen_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/lumen.sock",
            sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }

    /* Send hello */
    lumen_hello_t hello = { LUMEN_MAGIC, LUMEN_VERSION };
    if (write(fd, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        close(fd); return -EIO;
    }

    /* Receive reply */
    lumen_hello_reply_t reply;
    if (read(fd, &reply, sizeof(reply)) != (ssize_t)sizeof(reply)) {
        close(fd); return -EIO;
    }
    if (reply.magic != LUMEN_MAGIC || reply.status != 0) {
        close(fd); return -EPROTO;
    }

    return fd;
}
```

- [ ] **Step 5: Wire `lumen_server_init` + `lumen_server_tick` into `lumen/main.c`**

Add `#include "lumen_server.h"` near the top of `main.c` (after the existing includes).

In `main()`, after `comp_init(...)` and before the crossfade:
```c
    /* Start external window server */
    int lumen_srv_fd = lumen_server_init();
    if (lumen_srv_fd < 0)
        dprintf(2, "[LUMEN] warning: could not open /run/lumen.sock\n");
```

In the main event loop (`for (;;) {`), at the top of the loop body, before the keyboard poll:
```c
        /* Service external window clients */
        if (lumen_srv_fd >= 0) {
            if (lumen_server_tick(&comp, lumen_srv_fd))
                activity = 1;
        }
```

- [ ] **Step 6: Verify it compiles**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 \
  "cd ~/Developer/aegis && make -C user/bin/lumen 2>&1 | tail -10"
```
Expected: compiles with no errors.

- [ ] **Step 7: Commit**

```bash
git add user/lib/glyph/lumen_client.c user/bin/lumen/lumen_server.c \
        user/bin/lumen/main.c
git commit -m "lumen: server socket init/handshake + client lumen_connect"
```

---

## Task 3: Server-side `CREATE_WINDOW` — proxy_window_t + memfd + comp_add_window

**Files:**
- Modify: `user/bin/lumen/lumen_server.c` — implement proxy callbacks + `handle_create_window`

- [ ] **Step 1: Add proxy callback stubs above `lumen_server_accept_fd`**

```c
/* ── Proxy window callbacks ─────────────────────────────────────────── */

static void proxy_on_render(glyph_window_t *win)
{
    proxy_window_t *pw = win->priv;
    int client_w  = win->client_w;
    int client_h  = win->client_h;
    int surf_pitch = win->surface.pitch;  /* pitch in pixels */

    /* Blit shared buffer (stride = client_w) into surface client area.
     * Client area starts at (GLYPH_BORDER_WIDTH, GLYPH_TITLEBAR_HEIGHT +
     * GLYPH_BORDER_WIDTH) within the full surface. */
    uint32_t *dst = win->surface.buf
                    + (GLYPH_TITLEBAR_HEIGHT + GLYPH_BORDER_WIDTH) * surf_pitch
                    + GLYPH_BORDER_WIDTH;
    const uint32_t *src = pw->shared;

    for (int row = 0; row < client_h; row++)
        memcpy(dst + row * surf_pitch,
               src + row * client_w,
               (size_t)client_w * sizeof(uint32_t));
}

static void proxy_on_close(glyph_window_t *win)
{
    proxy_window_t *pw = win->priv;
    lumen_msg_hdr_t hdr = { LUMEN_EV_CLOSE_REQUEST,
                             sizeof(lumen_close_request_t) };
    lumen_close_request_t ev = { pw->id };
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
    /* Do NOT destroy the window — wait for client to send DESTROY_WINDOW */
}

static void proxy_on_key(glyph_window_t *win, char key)
{
    proxy_window_t *pw = win->priv;
    lumen_msg_hdr_t hdr = { LUMEN_EV_KEY, sizeof(lumen_key_event_t) };
    lumen_key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window_id = pw->id;
    ev.keycode   = (uint32_t)(uint8_t)key;
    ev.pressed   = 1;
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

/* Mouse callbacks receive window-relative coords (including chrome).
 * Subtract chrome to get client-area-relative before sending. */
static void send_mouse_event(proxy_window_t *pw, int win_x, int win_y,
                              uint8_t buttons, uint8_t evtype)
{
    int cx = win_x - GLYPH_BORDER_WIDTH;
    int cy = win_y - GLYPH_TITLEBAR_HEIGHT - GLYPH_BORDER_WIDTH;
    lumen_msg_hdr_t hdr = { LUMEN_EV_MOUSE, sizeof(lumen_mouse_event_t) };
    lumen_mouse_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window_id = pw->id;
    ev.x         = cx;
    ev.y         = cy;
    ev.buttons   = buttons;
    ev.evtype    = evtype;
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}

static void proxy_on_mouse_down(glyph_window_t *win, int x, int y)
{
    send_mouse_event(win->priv, x, y, 1, LUMEN_MOUSE_DOWN);
}

static void proxy_on_mouse_move(glyph_window_t *win, int x, int y)
{
    send_mouse_event(win->priv, x, y, 0, LUMEN_MOUSE_MOVE);
}

static void proxy_on_mouse_up(glyph_window_t *win, int x, int y)
{
    send_mouse_event(win->priv, x, y, 0, LUMEN_MOUSE_UP);
}
```

- [ ] **Step 2: Add `handle_create_window` helper in `lumen_server.c`**

```c
static int handle_create_window(compositor_t *comp, lumen_client_t *cli,
                                  const lumen_create_window_t *req)
{
    if (cli->nwindows >= LUMEN_MAX_WINDOWS_PER_CLIENT)
        goto err_reply;

    int w = req->width;
    int h = req->height;
    size_t bufsz = (size_t)w * h * sizeof(uint32_t);

    /* Shared pixel buffer via memfd */
    int memfd = memfd_create("lumen_win", 0);
    if (memfd < 0) goto err_reply;
    if (ftruncate(memfd, (off_t)bufsz) < 0) { close(memfd); goto err_reply; }

    /* Lumen maps PROT_READ — client writes, Lumen reads */
    void *shared = mmap(NULL, bufsz, PROT_READ, MAP_SHARED, memfd, 0);
    if (shared == MAP_FAILED) { close(memfd); goto err_reply; }

    proxy_window_t *pw = calloc(1, sizeof(*pw));
    if (!pw) { munmap(shared, bufsz); close(memfd); goto err_reply; }

    pw->client = cli;
    pw->id     = cli->next_id++;
    pw->memfd  = memfd;
    pw->shared = shared;

    /* Title (safe copy) */
    char title[64];
    memset(title, 0, sizeof(title));
    strncpy(title, req->title, sizeof(title) - 1);

    pw->win = glyph_window_create(title, w, h);
    if (!pw->win) { free(pw); munmap(shared, bufsz); close(memfd); goto err_reply; }

    pw->win->priv          = pw;
    pw->win->on_render     = proxy_on_render;
    pw->win->on_close      = proxy_on_close;
    pw->win->on_key        = proxy_on_key;
    pw->win->on_mouse_down = proxy_on_mouse_down;
    pw->win->on_mouse_move = proxy_on_mouse_move;
    pw->win->on_mouse_up   = proxy_on_mouse_up;

    /* Center on screen */
    pw->win->x = (comp->fb.w - pw->win->surf_w) / 2;
    pw->win->y = (comp->fb.h - pw->win->surf_h) / 2;

    comp_add_window(comp, pw->win);
    glyph_window_mark_all_dirty(pw->win);
    comp->full_redraw = 1;

    cli->windows[cli->nwindows++] = pw;

    /* Reply: send lumen_window_created_t + memfd via SCM_RIGHTS */
    lumen_window_created_t reply_data = {
        .status    = 0,
        .window_id = pw->id,
        .width     = (uint32_t)w,
        .height    = (uint32_t)h,
    };
    lumen_msg_hdr_t rhdr = { 0, sizeof(reply_data) };

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov[2] = {
        { .iov_base = &rhdr,       .iov_len = sizeof(rhdr)       },
        { .iov_base = &reply_data, .iov_len = sizeof(reply_data) },
    };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = iov;
    msg.msg_iovlen     = 2;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &memfd, sizeof(int));
    msg.msg_controllen = cmsg->cmsg_len;

    sendmsg(cli->fd, &msg, 0);
    return 1;  /* window added — compositor dirty */

err_reply: {
        lumen_window_created_t err_data = { (uint32_t)EIO, 0, 0, 0 };
        lumen_msg_hdr_t ehdr = { 0, sizeof(err_data) };
        write(cli->fd, &ehdr,     sizeof(ehdr));
        write(cli->fd, &err_data, sizeof(err_data));
        return 0;
    }
}
```

- [ ] **Step 3: Verify it compiles**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 \
  "cd ~/Developer/aegis && make -C user/bin/lumen 2>&1 | tail -10"
```
Expected: compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add user/bin/lumen/lumen_server.c
git commit -m "lumen: proxy window callbacks + handle_create_window with memfd+SCM_RIGHTS"
```

---

## Task 4: Client-side `CREATE_WINDOW` + `lumen_window_present` + `DAMAGE` + server wires it up

**Files:**
- Modify: `user/lib/glyph/lumen_client.c` — implement `lumen_window_create`, `lumen_window_present`
- Modify: `user/bin/lumen/lumen_server.c` — implement real `lumen_server_read` + `handle_damage`

- [ ] **Step 1: Implement `lumen_window_create` in `lumen_client.c`**

Add static helper (internal receive for event reading — also used by lumen_wait_event):
```c
static int recv_event(int fd, lumen_event_t *ev)
{
    lumen_msg_hdr_t hdr;
    ssize_t n = read(fd, &hdr, sizeof(hdr));
    if (n == 0 || n < 0) return -1;

    memset(ev, 0, sizeof(*ev));
    ev->type = hdr.op;

    switch (hdr.op) {
    case LUMEN_EV_KEY: {
        lumen_key_event_t k;
        if (read(fd, &k, sizeof(k)) != (ssize_t)sizeof(k)) return -1;
        ev->window_id     = k.window_id;
        ev->key.keycode   = k.keycode;
        ev->key.modifiers = k.modifiers;
        ev->key.pressed   = k.pressed;
        break;
    }
    case LUMEN_EV_MOUSE: {
        lumen_mouse_event_t m;
        if (read(fd, &m, sizeof(m)) != (ssize_t)sizeof(m)) return -1;
        ev->window_id      = m.window_id;
        ev->mouse.x        = m.x;
        ev->mouse.y        = m.y;
        ev->mouse.buttons  = m.buttons;
        ev->mouse.evtype   = m.evtype;
        break;
    }
    case LUMEN_EV_CLOSE_REQUEST: {
        lumen_close_request_t c;
        if (read(fd, &c, sizeof(c)) != (ssize_t)sizeof(c)) return -1;
        ev->window_id = c.window_id;
        break;
    }
    case LUMEN_EV_FOCUS: {
        lumen_focus_event_t f;
        if (read(fd, &f, sizeof(f)) != (ssize_t)sizeof(f)) return -1;
        ev->window_id    = f.window_id;
        ev->focus.focused = f.focused;
        break;
    }
    case LUMEN_EV_RESIZED: {
        lumen_resized_event_t r;
        if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) return -1;
        ev->window_id     = r.window_id;
        ev->resized.new_w = r.new_width;
        ev->resized.new_h = r.new_height;
        break;
    }
    default: {
        /* Unknown opcode — drain and skip */
        char tmp[256];
        uint32_t rem = hdr.len;
        while (rem > 0) {
            ssize_t r = read(fd, tmp,
                             rem < (uint32_t)sizeof(tmp) ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        ev->type = 0;  /* signal caller to ignore */
        break;
    }
    }
    return 1;
}
```

Replace `lumen_window_create` stub:
```c
lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h)
{
    /* Send CREATE_WINDOW */
    lumen_msg_hdr_t hdr = { LUMEN_OP_CREATE_WINDOW,
                             sizeof(lumen_create_window_t) };
    lumen_create_window_t req;
    memset(&req, 0, sizeof(req));
    req.width  = (uint16_t)w;
    req.height = (uint16_t)h;
    strncpy(req.title, title ? title : "", sizeof(req.title) - 1);

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return NULL;
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return NULL;

    /* Receive reply (framed header + lumen_window_created_t) + memfd via SCM_RIGHTS */
    lumen_msg_hdr_t rhdr;
    lumen_window_created_t created;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov[2] = {
        { .iov_base = &rhdr,    .iov_len = sizeof(rhdr)    },
        { .iov_base = &created, .iov_len = sizeof(created) },
    };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = iov;
    msg.msg_iovlen     = 2;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (recvmsg(fd, &msg, 0) < 0) return NULL;
    if (created.status != 0) return NULL;

    /* Extract memfd */
    int memfd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&memfd, CMSG_DATA(cmsg), sizeof(int));
    if (memfd < 0) return NULL;

    size_t bufsz = (size_t)created.width * created.height * sizeof(uint32_t);

    /* Map shared buffer (PROT_READ|WRITE — client renders here) */
    void *shared = mmap(NULL, bufsz, PROT_READ | PROT_WRITE,
                        MAP_SHARED, memfd, 0);
    if (shared == MAP_FAILED) { close(memfd); return NULL; }

    void *backbuf = malloc(bufsz);
    if (!backbuf) { munmap(shared, bufsz); close(memfd); return NULL; }
    memset(backbuf, 0, bufsz);

    lumen_window_t *win = malloc(sizeof(*win));
    if (!win) { free(backbuf); munmap(shared, bufsz); close(memfd); return NULL; }

    win->fd      = fd;
    win->id      = created.window_id;
    win->memfd   = memfd;
    win->shared  = shared;
    win->backbuf = backbuf;
    win->w       = (int)created.width;
    win->h       = (int)created.height;
    win->stride  = (int)created.width;

    return win;
}
```

- [ ] **Step 2: Implement `lumen_window_present`, `lumen_poll_event`, `lumen_wait_event`, `lumen_window_destroy`**

```c
void lumen_window_present(lumen_window_t *win)
{
    size_t bufsz = (size_t)win->w * win->h * sizeof(uint32_t);
    memcpy(win->shared, win->backbuf, bufsz);

    lumen_msg_hdr_t hdr = { LUMEN_OP_DAMAGE, sizeof(lumen_damage_t) };
    lumen_damage_t dmg  = { win->id };
    write(win->fd, &hdr, sizeof(hdr));
    write(win->fd, &dmg, sizeof(dmg));
}

int lumen_poll_event(int fd, lumen_event_t *ev)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0) return 0;
    return recv_event(fd, ev);
}

int lumen_wait_event(int fd, lumen_event_t *ev, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0)  return -1;
    return recv_event(fd, ev);
}

void lumen_window_destroy(lumen_window_t *win)
{
    lumen_msg_hdr_t hdr  = { LUMEN_OP_DESTROY_WINDOW,
                              sizeof(lumen_destroy_window_t) };
    lumen_destroy_window_t req = { win->id };
    write(win->fd, &hdr, sizeof(hdr));
    write(win->fd, &req, sizeof(req));

    size_t bufsz = (size_t)win->w * win->h * sizeof(uint32_t);
    munmap(win->shared, bufsz);
    close(win->memfd);
    free(win->backbuf);
    free(win);
}
```

- [ ] **Step 3: Implement real `lumen_server_read` in `lumen_server.c`**

Remove the stub. Add `find_proxy` helper and full `lumen_server_read`:

```c
static proxy_window_t *find_proxy(lumen_client_t *cli, uint32_t id)
{
    for (int i = 0; i < cli->nwindows; i++)
        if (cli->windows[i]->id == id)
            return cli->windows[i];
    return NULL;
}

static int handle_damage(compositor_t *comp, lumen_client_t *cli,
                           uint32_t window_id)
{
    proxy_window_t *pw = find_proxy(cli, window_id);
    if (!pw) return 0;
    glyph_window_mark_all_dirty(pw->win);
    comp->full_redraw = 1;
    return 1;
}

static int handle_destroy_window(compositor_t *comp, lumen_client_t *cli,
                                   uint32_t window_id)
{
    for (int i = 0; i < cli->nwindows; i++) {
        proxy_window_t *pw = cli->windows[i];
        if (pw->id != window_id) continue;

        comp_remove_window(comp, pw->win);
        comp->full_redraw = 1;

        size_t bufsz = (size_t)pw->win->client_w * pw->win->client_h
                       * sizeof(uint32_t);
        munmap(pw->shared, bufsz);
        close(pw->memfd);
        glyph_window_destroy(pw->win);
        free(pw);

        cli->windows[i] = cli->windows[--cli->nwindows];
        return 1;
    }
    return 0;
}

static int lumen_server_read(compositor_t *comp, lumen_client_t *cli)
{
    lumen_msg_hdr_t hdr;
    ssize_t n = read(cli->fd, &hdr, sizeof(hdr));
    if (n == 0) return -1;   /* EOF */
    if (n < 0)  return (errno == EAGAIN) ? 0 : -1;
    if (n != (ssize_t)sizeof(hdr)) return -1;

    switch (hdr.op) {
    case LUMEN_OP_CREATE_WINDOW: {
        lumen_create_window_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_create_window(comp, cli, &req);
    }
    case LUMEN_OP_DAMAGE: {
        lumen_damage_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_damage(comp, cli, req.window_id);
    }
    case LUMEN_OP_SET_TITLE: {
        lumen_set_title_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        /* Silently accepted in v1 — title updates not yet reflected in chrome */
        return 0;
    }
    case LUMEN_OP_DESTROY_WINDOW: {
        lumen_destroy_window_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_destroy_window(comp, cli, req.window_id);
    }
    default: {
        /* Drain unknown payload */
        char tmp[256];
        uint32_t rem = hdr.len;
        while (rem > 0) {
            ssize_t r = read(cli->fd, tmp,
                             rem < (uint32_t)sizeof(tmp)
                             ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        return 0;
    }
    }
}
```

- [ ] **Step 4: Implement real `lumen_server_hangup` in `lumen_server.c`**

Replace the stub:
```c
static void lumen_server_hangup(compositor_t *comp, lumen_client_t *cli)
{
    for (int i = 0; i < cli->nwindows; i++) {
        proxy_window_t *pw = cli->windows[i];
        comp_remove_window(comp, pw->win);
        size_t bufsz = (size_t)pw->win->client_w * pw->win->client_h
                       * sizeof(uint32_t);
        munmap(pw->shared, bufsz);
        close(pw->memfd);
        glyph_window_destroy(pw->win);
        free(pw);
    }
    if (cli->nwindows > 0)
        comp->full_redraw = 1;

    close(cli->fd);

    /* Remove from s_clients by swap-with-last */
    for (int i = 0; i < s_ncli; i++) {
        if (s_clients[i] == cli) {
            s_clients[i] = s_clients[--s_ncli];
            break;
        }
    }
    free(cli);
}
```

- [ ] **Step 5: Build both sides**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 \
  "cd ~/Developer/aegis && make -C user/lib/glyph && make -C user/bin/lumen 2>&1 | tail -10"
```
Expected: compiles with no errors.

- [ ] **Step 6: Smoke test — write a minimal test client**

On the build box, create `/tmp/lumen_test.c`:
```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 "cat > /tmp/lumen_test.c" << 'EOF'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "lumen_client.h"

int main(void)
{
    int fd = lumen_connect();
    if (fd < 0) { fprintf(stderr, "connect failed: %d\n", fd); return 1; }
    fprintf(stderr, "connected, fd=%d\n", fd);

    lumen_window_t *win = lumen_window_create(fd, "Test Window", 400, 300);
    if (!win) { fprintf(stderr, "create_window failed\n"); return 1; }
    fprintf(stderr, "window created: id=%u %dx%d\n", win->id, win->w, win->h);

    /* Fill backbuf with solid blue, present */
    uint32_t *px = win->backbuf;
    for (int i = 0; i < win->w * win->h; i++)
        px[i] = 0x000000FFu;
    lumen_window_present(win);

    fprintf(stderr, "presented frame, sleeping 3s...\n");
    sleep(3);

    lumen_window_destroy(win);
    return 0;
}
EOF
```

This smoke test is for manual verification during a full ISO boot (Task 9). For now, just verify the code compiles in isolation — full integration testing requires a running Lumen.

- [ ] **Step 7: Commit**

```bash
git add user/lib/glyph/lumen_client.c user/bin/lumen/lumen_server.c
git commit -m "lumen: client window create/present/destroy + full server dispatch"
```

---

## Task 5: Input events — KEY, MOUSE, CLOSE_REQUEST server→client

The proxy callbacks for key, mouse, and close were already added in Task 3. This task verifies they are wired up correctly and adds a focus event when Lumen changes focused window.

**Files:**
- Modify: `user/bin/lumen/lumen_server.c` — add `send_focus_event` helper; wire into comp focus changes
- Modify: `user/bin/lumen/compositor.c` — call `lumen_proxy_focus_changed` when focus changes (optional — focus events are nice-to-have in v1)

- [ ] **Step 1: Add `send_focus_event` helper in `lumen_server.c`**

```c
/* Called by compositor when a proxy window gains or loses focus.
 * Public so compositor.c can call it. */
void lumen_proxy_notify_focus(glyph_window_t *win, int focused)
{
    if (!win || !win->priv) return;
    proxy_window_t *pw = win->priv;
    /* Only send if this is actually a proxy window (check on_render) */
    if (win->on_render != proxy_on_render) return;
    lumen_msg_hdr_t hdr = { LUMEN_EV_FOCUS, sizeof(lumen_focus_event_t) };
    lumen_focus_event_t ev = { pw->id, (uint8_t)focused, {0,0,0} };
    write(pw->client->fd, &hdr, sizeof(hdr));
    write(pw->client->fd, &ev,  sizeof(ev));
}
```

Add declaration to `lumen_server.h`:
```c
/* Notify a proxy window of focus change. win may be NULL or non-proxy (no-op). */
void lumen_proxy_notify_focus(glyph_window_t *win, int focused);
```

- [ ] **Step 2: Verify proxy callbacks are invoked correctly**

The proxy callbacks `proxy_on_key`, `proxy_on_close`, `proxy_on_mouse_*` are already set on `pw->win` in `handle_create_window`. Lumen's existing dispatch (`comp_handle_key`, `glyph_window_dispatch_key`, `comp_handle_mouse`) calls them automatically. No extra wiring needed.

Check `compositor.c` confirms `on_mouse_down` is called when `win->priv` is set (line 640):
```
/* Content area mouse-down: start content drag for text selection.
 * Only for terminal windows (have on_mouse_down), not widget windows. */
if (win->on_mouse_down && win->priv) {
    win->on_mouse_down(win, local_x, local_y);
```

Proxy windows have `priv != NULL` and `on_mouse_down = proxy_on_mouse_down`, so this fires. Good.

- [ ] **Step 3: Compile check**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 \
  "cd ~/Developer/aegis && make -C user/bin/lumen 2>&1 | tail -5"
```

- [ ] **Step 4: Commit**

```bash
git add user/bin/lumen/lumen_server.c user/bin/lumen/lumen_server.h
git commit -m "lumen: focus event helper + input event routing verified"
```

---

## Task 6: `DESTROY_WINDOW` + `lumen_window_destroy` + hangup cleanup (already implemented — verification only)

`DESTROY_WINDOW` server handling, `lumen_window_destroy` client side, and `lumen_server_hangup` were all implemented in Task 4. This task builds and tests them.

**Files:**
- No new code — verification only

- [ ] **Step 1: Build ISO and boot on QEMU with GUI**

On build box:
```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 "cd ~/Developer/aegis && \
  git clean -fdx --exclude=references --exclude=.worktrees && \
  rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf && \
  make clean && make iso 2>&1 | tail -20"
```

- [ ] **Step 2: Run QEMU with GUI mode to verify Lumen starts without crashing**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 "cd ~/Developer/aegis && \
  timeout 60 qemu-system-x86_64 -machine q35 -cpu host -enable-kvm \
    -m 2G -cdrom build/aegis.iso -boot order=d \
    -display none -vga std -nodefaults -serial stdio -no-reboot \
    2>&1 | head -60"
```

Look for `[LUMEN] ready` in output. No panics.

- [ ] **Step 3: Verify `lumen_server_tick` compiles with full implementation**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 \
  "cd ~/Developer/aegis && make -C user/bin/lumen 2>&1"
```
Expected: 0 warnings, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add user/bin/lumen/lumen_server.c user/bin/lumen/lumen_server.h \
        user/lib/glyph/lumen_client.c user/lib/glyph/lumen_client.h \
        user/lib/glyph/lumen_proto.h user/lib/glyph/Makefile \
        user/bin/lumen/Makefile user/bin/lumen/main.c
git commit -m "lumen: external window protocol complete — server+client ready"
```

---

## Task 7: GUI installer port

**Files:**
- Modify: `user/bin/gui-installer/main.c`

The installer currently:
1. Calls `syscall(SYS_FB_MAP, &fb_info)` to map the raw framebuffer
2. Allocates a `backbuf`, sets up `g_st.surf` pointing to it
3. Reads keys from fd 0 in raw termios mode
4. Blits backbuf → fb with `memcpy`

After port:
1. Calls `lumen_connect()` + `lumen_window_create()` — gets a window with its own backbuf
2. `g_st.surf` points to `(uint32_t *)win->backbuf` with `win->w`, `win->h`
3. Gets input via `lumen_wait_event()`
4. Blits by calling `lumen_window_present()`

The five screen draw functions (`draw_screen_welcome`, etc.) are untouched — they render to `g_st.surf` which still exists.

- [ ] **Step 1: Update the `g_st` struct — remove raw FB fields, add lumen fields**

Find the `g_st` typedef in `main.c`. It currently has:

```c
    /* Framebuffer */
    fb_info_t  fb_info;
    uint32_t  *fb;
    uint32_t  *backbuf;
    int        fb_w, fb_h, pitch_px;
    surface_t  surf;
```

Replace with:
```c
    /* Lumen window */
    int            lfd;   /* socket fd from lumen_connect() */
    lumen_window_t *lwin;
    uint32_t  *backbuf;  /* == (uint32_t *)lwin->backbuf */
    int        fb_w, fb_h, pitch_px;
    surface_t  surf;
```

- [ ] **Step 2: Add `#include "lumen_client.h"` to the top of `main.c`**

After the existing `#include <glyph.h>` line, add:
```c
#include <lumen_client.h>
```

Remove `#define SYS_FB_MAP 513` — no longer needed.

- [ ] **Step 3: Update `blit()` to use `lumen_window_present`**

Find:
```c
static void blit(void)
{
    memcpy(g_st.fb, g_st.backbuf,
           (size_t)g_st.pitch_px * g_st.fb_h * 4);
}
```

Replace with:
```c
static void blit(void)
{
    lumen_window_present(g_st.lwin);
}
```

- [ ] **Step 4: Replace the FB init block in `main()`**

Find:
```c
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
```

Replace with:
```c
    /* Connect to Lumen compositor */
    g_st.lfd = lumen_connect();
    if (g_st.lfd < 0) {
        dprintf(2, "gui-installer: lumen_connect failed (%d)\n", g_st.lfd);
        return 1;
    }
    g_st.lwin = lumen_window_create(g_st.lfd, "Install Aegis", 800, 600);
    if (!g_st.lwin) {
        dprintf(2, "gui-installer: lumen_window_create failed\n");
        return 1;
    }
    g_st.backbuf  = (uint32_t *)g_st.lwin->backbuf;
    g_st.fb_w     = g_st.lwin->w;
    g_st.fb_h     = g_st.lwin->h;
    g_st.pitch_px = g_st.lwin->stride;
    g_st.surf = (surface_t){
        .buf   = g_st.backbuf,
        .w     = g_st.fb_w,
        .h     = g_st.fb_h,
        .pitch = g_st.pitch_px,
    };
```

- [ ] **Step 5: Remove raw termios setup**

Find and delete the raw keyboard setup block:
```c
    /* Raw keyboard */
    struct termios t_orig, t_raw;
    tcgetattr(0, &t_orig);
    t_raw = t_orig;
    t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
    t_raw.c_cc[VMIN]  = 0;
    t_raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t_raw);
```

Also remove the `tcsetattr(0, TCSANOW, &t_orig);` restore at the end of `main()`.

Also remove `#include <termios.h>` if it's no longer used elsewhere in the file.

- [ ] **Step 6: Replace the event loop**

Find:
```c
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
```

Replace with:
```c
    /* Event loop */
    while (!s_term_requested) {
        lumen_event_t ev;
        /* Block up to 16ms waiting for input — also redraws on timeout */
        int r = lumen_wait_event(g_st.lfd, &ev, 16);
        if (r < 0) break;  /* Lumen died */

        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST)
                break;
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed)
                handle_key((char)ev.key.keycode);
        }

        render_current_screen();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    return 0;
```

- [ ] **Step 7: Build the installer**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 \
  "cd ~/Developer/aegis && make -C user/bin/gui-installer 2>&1 | tail -10"
```
Expected: compiles with no errors.

- [ ] **Step 8: Commit**

```bash
git add user/bin/gui-installer/main.c
git commit -m "gui-installer: port from raw framebuffer to Lumen external window protocol"
```

---

## Task 8: Capability cleanup + make test

**Files:**
- Modify: `rootfs/etc/aegis/caps.d/gui-installer`

- [ ] **Step 1: Remove `FB` capability from `gui-installer` caps file**

Read the current file:
```bash
cat /Users/dylan/Developer/aegis/rootfs/etc/aegis/caps.d/gui-installer
```
Current content: `admin DISK_ADMIN AUTH FB`

Change to: `admin DISK_ADMIN AUTH`

(FB is no longer needed — the installer no longer calls `SYS_FB_MAP`.)

- [ ] **Step 2: Full nuclear clean + ISO build on x86 build box**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 "cd ~/Developer/aegis && \
  git pull && \
  git clean -fdx --exclude=references --exclude=.worktrees && \
  rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf && \
  make clean && make iso 2>&1 | tail -20"
```
Expected: `aegis.iso` built with no errors.

- [ ] **Step 3: Run `make test` — verify boot oracle passes**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 \
  "cd ~/Developer/aegis && AEGIS_BOOT_TIMEOUT=120 make test 2>&1 | tail -20"
```
Expected: exit 0, `[TEST] PASS`.

- [ ] **Step 4: Manual GUI verification in QEMU**

Boot QEMU with GUI on the build box:
```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 "cd ~/Developer/aegis && \
  qemu-system-x86_64 -machine q35 -cpu host -enable-kvm \
    -m 2G -cdrom build/aegis.iso -boot order=d \
    -display vnc=:1 -vga std -nodefaults -serial stdio -no-reboot 2>&1 &"
```
Connect with VNC and verify:
1. Lumen desktop appears normally.
2. Click the "Install Aegis" desktop icon — installer opens as a normal Lumen window with title bar.
3. Installer renders its welcome screen inside the window.
4. Another terminal window can be opened and dragged over the installer — installer composites correctly under/over.
5. Press keys in the installer — screens navigate correctly.
6. Click the window close button — installer exits cleanly.
7. Kill the installer with `kill -9` from another terminal — no zombie window remains.

- [ ] **Step 5: Final commit**

```bash
git add rootfs/etc/aegis/caps.d/gui-installer
git commit -m "gui-installer: remove CAP_KIND_FB (no longer maps framebuffer directly)"
```

---

## Self-Review

**Spec coverage check:**
- §3 Architecture: covered — AF_UNIX socket, memfd, proxy_window_t, comp_add_window. ✓
- §4 Wire protocol: all structs defined in lumen_proto.h. All opcodes handled in lumen_server_read. ✓
- §5 Client library: all functions implemented in lumen_client.c. ✓
- §6.2 /run/ mkdir: done in lumen_server_init. ✓
- §6.5 on_close subtlety: proxy_on_close sends CLOSE_REQUEST, does NOT destroy. ✓
- §6.6 Client death cleanup: lumen_server_hangup handles all windows + free. ✓
- §6.7 Event loop integration: lumen_server_tick called in main.c loop. ✓
- §7 Installer port: all four changes (fb_map → lumen, kbd → wait_event, blit → present, caps). ✓

**Placeholder scan:** No TBDs or incomplete steps. All code blocks contain real code.

**Type consistency:**
- `proxy_on_render(glyph_window_t *win)` — matches `void (*on_render)(glyph_window_t *self)` in glyph.h:133. ✓
- `proxy_on_key(glyph_window_t *win, char key)` — matches `void (*on_key)(glyph_window_t *self, char key)` in glyph.h:131. ✓
- `proxy_on_mouse_down(glyph_window_t *win, int x, int y)` — matches `void (*on_mouse_down)(glyph_window_t *self, int x, int y)` in glyph.h:134. ✓
- `glyph_window_destroy(pw->win)` — correct function name (glyph.h:143). ✓
- `win->surface.buf` is `uint32_t *` (draw.h:12). ✓
- `win->surface.pitch` is pitch in pixels (draw.h:13). ✓
- `GLYPH_TITLEBAR_HEIGHT = 30`, `GLYPH_BORDER_WIDTH = 1` (glyph.h:94-95). ✓
- `comp_remove_window(compositor_t *c, glyph_window_t *win)` — exists in compositor.h:57. ✓
- `lumen_window_t.backbuf` is `void *` — cast to `uint32_t *` in installer. ✓
