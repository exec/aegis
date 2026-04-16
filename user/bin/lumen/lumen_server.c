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

#define LUMEN_MAX_CLIENTS            8
#define LUMEN_MAX_WINDOWS_PER_CLIENT 8

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
    void           *shared;
};

static lumen_client_t *s_clients[LUMEN_MAX_CLIENTS];
static int              s_ncli;

/* ── Proxy window callbacks ─────────────────────────────────────────── */

static void proxy_on_render(glyph_window_t *win)
{
    proxy_window_t *pw = win->priv;
    int client_w  = win->client_w;
    int client_h  = win->client_h;
    int surf_pitch = win->surface.pitch;

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

/* ── CREATE_WINDOW handler ──────────────────────────────────────────── */

static int handle_create_window(compositor_t *comp, lumen_client_t *cli,
                                  const lumen_create_window_t *req)
{
    if (cli->nwindows >= LUMEN_MAX_WINDOWS_PER_CLIENT)
        goto err_reply;

    int w = req->width;
    int h = req->height;
    size_t bufsz = (size_t)w * h * sizeof(uint32_t);

    int memfd = memfd_create("lumen_win", 0);
    if (memfd < 0) goto err_reply;
    if (ftruncate(memfd, (off_t)bufsz) < 0) { close(memfd); goto err_reply; }

    void *shared = mmap(NULL, bufsz, PROT_READ, MAP_SHARED, memfd, 0);
    if (shared == MAP_FAILED) { close(memfd); goto err_reply; }

    proxy_window_t *pw = calloc(1, sizeof(*pw));
    if (!pw) { munmap(shared, bufsz); close(memfd); goto err_reply; }

    pw->client = cli;
    pw->id     = cli->next_id++;
    pw->memfd  = memfd;
    pw->shared = shared;

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

    pw->win->x = (comp->fb.w - pw->win->surf_w) / 2;
    pw->win->y = (comp->fb.h - pw->win->surf_h) / 2;

    comp_add_window(comp, pw->win);
    glyph_window_mark_all_dirty(pw->win);
    comp->full_redraw = 1;

    cli->windows[cli->nwindows++] = pw;

    /* Reply: lumen_window_created_t + memfd via SCM_RIGHTS */
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
    return 1;

err_reply: {
        lumen_window_created_t err_data = { (uint32_t)EIO, 0, 0, 0 };
        lumen_msg_hdr_t ehdr = { 0, sizeof(err_data) };
        write(cli->fd, &ehdr,     sizeof(ehdr));
        write(cli->fd, &err_data, sizeof(err_data));
        return 0;
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

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

/* ── Client read + hangup ───────────────────────────────────────────── */

static int lumen_server_read(compositor_t *comp, lumen_client_t *cli)
{
    lumen_msg_hdr_t hdr;
    ssize_t n = read(cli->fd, &hdr, sizeof(hdr));
    if (n == 0) return -1;
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
        return 0;
    }
    case LUMEN_OP_DESTROY_WINDOW: {
        lumen_destroy_window_t req;
        if (read(cli->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return -1;
        return handle_destroy_window(comp, cli, req.window_id);
    }
    default: {
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

    for (int i = 0; i < s_ncli; i++) {
        if (s_clients[i] == cli) {
            s_clients[i] = s_clients[--s_ncli];
            break;
        }
    }
    free(cli);
}

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

int lumen_server_init(void)
{
    mkdir("/run", 0755);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

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

    fcntl(fd, F_SETFL, O_NONBLOCK);

    return fd;
}

int lumen_server_tick(compositor_t *comp, int listen_fd)
{
    int dirtied = 0;

    {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0)
            lumen_server_accept_fd(comp, listen_fd);
    }

    for (int i = 0; i < s_ncli; ) {
        struct pollfd pfd = { .fd = s_clients[i]->fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            int r = lumen_server_read(comp, s_clients[i]);
            if (r < 0) {
                lumen_server_hangup(comp, s_clients[i]);
                continue;
            }
            if (r > 0)
                dirtied = 1;
        }
        i++;
    }

    return dirtied;
}
