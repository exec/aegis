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

    lumen_hello_t hello = { LUMEN_MAGIC, LUMEN_VERSION };
    if (write(fd, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        close(fd); return -EIO;
    }

    lumen_hello_reply_t reply;
    if (read(fd, &reply, sizeof(reply)) != (ssize_t)sizeof(reply)) {
        close(fd); return -EIO;
    }
    if (reply.magic != LUMEN_MAGIC || reply.status != 0) {
        close(fd); return -EPROTO;
    }

    return fd;
}

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
        char tmp[256];
        uint32_t rem = hdr.len;
        while (rem > 0) {
            ssize_t r = read(fd, tmp,
                             rem < (uint32_t)sizeof(tmp) ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        ev->type = 0;
        break;
    }
    }
    return 1;
}

lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_CREATE_WINDOW,
                             sizeof(lumen_create_window_t) };
    lumen_create_window_t req;
    memset(&req, 0, sizeof(req));
    req.width  = (uint16_t)w;
    req.height = (uint16_t)h;
    strncpy(req.title, title ? title : "", sizeof(req.title) - 1);

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return NULL;
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) return NULL;

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

    int memfd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&memfd, CMSG_DATA(cmsg), sizeof(int));
    if (memfd < 0) return NULL;

    size_t bufsz = (size_t)created.width * created.height * sizeof(uint32_t);

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
