/* kernel/net/unix_socket.c — AF_UNIX domain sockets */
#include "unix_socket.h"
#include "proc.h"
#include "printk.h"
#include "kva.h"
#include "spinlock.h"
#include <stdint.h>

/* ── Static tables ─────────────────────────────────────────────────────── */

static unix_sock_t s_unix[UNIX_SOCK_MAX];
static spinlock_t  unix_lock = SPINLOCK_INIT;

/* Name table: path → sock_id for bound sockets */
#define UNIX_NAME_MAX 32
typedef struct {
    char     path[UNIX_PATH_MAX];
    uint32_t sock_id;
    uint8_t  in_use;
} unix_name_t;

static unix_name_t s_names[UNIX_NAME_MAX];

/* ── Local helpers ─────────────────────────────────────────────────────── */

static void _memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
}

static int _streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void _strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static uint16_t ring_used(unix_sock_t *s)
{
    return (uint16_t)((s->ring_head - s->ring_tail) & (UNIX_BUF_SIZE - 1));
}

static uint16_t ring_free(unix_sock_t *s)
{
    return (uint16_t)(UNIX_BUF_SIZE - 1 - ring_used(s));
}

/* ── Name table ────────────────────────────────────────────────────────── */

static int name_register(const char *path, uint32_t sock_id)
{
    for (int i = 0; i < UNIX_NAME_MAX; i++) {
        if (s_names[i].in_use && _streq(s_names[i].path, path))
            return -98;  /* EADDRINUSE */
    }
    for (int i = 0; i < UNIX_NAME_MAX; i++) {
        if (!s_names[i].in_use) {
            _strcpy(s_names[i].path, path, UNIX_PATH_MAX);
            s_names[i].sock_id = sock_id;
            s_names[i].in_use  = 1;
            return 0;
        }
    }
    return -28;  /* ENOSPC */
}

static void name_unregister(const char *path)
{
    for (int i = 0; i < UNIX_NAME_MAX; i++) {
        if (s_names[i].in_use && _streq(s_names[i].path, path)) {
            s_names[i].in_use = 0;
            return;
        }
    }
}

static uint32_t name_lookup(const char *path)
{
    for (int i = 0; i < UNIX_NAME_MAX; i++) {
        if (s_names[i].in_use && _streq(s_names[i].path, path))
            return s_names[i].sock_id;
    }
    return UNIX_NONE;
}

/* ── VFS ops ───────────────────────────────────────────────────────────── */

/* VFS read/write ops receive user-space pointers from sys_read/sys_write.
 * With SMAP enabled, the kernel cannot access them directly. Bounce
 * through a stack buffer, same pattern as pipe and console VFS ops. */

#include "uaccess.h"

static int unix_vfs_read(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)off;
    uint32_t id = (uint32_t)(uintptr_t)priv;
    uint8_t kbuf[1024];
    uint32_t want = (uint32_t)len;
    if (want > 1024) want = 1024;
    int n = unix_sock_read(id, kbuf, want);
    if (n > 0)
        copy_to_user(buf, kbuf, (uint32_t)n);
    return n;
}

static int unix_vfs_write(void *priv, const void *buf, uint64_t len)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    uint8_t kbuf[1024];
    uint32_t want = (uint32_t)len;
    if (want > 1024) want = 1024;
    copy_from_user(kbuf, buf, want);
    return unix_sock_write(id, kbuf, want);
}

static void unix_vfs_close(void *priv)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    unix_sock_free(id);
}

static void unix_vfs_dup(void *priv)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    if (id < UNIX_SOCK_MAX && s_unix[id].in_use)
        s_unix[id].refcount++;
    spin_unlock_irqrestore(&unix_lock, fl);
}

static int unix_vfs_stat(void *priv, k_stat_t *st)
{
    (void)priv;
    _memset(st, 0, sizeof(*st));
    st->st_mode = 0140000U | 0666U;  /* S_IFSOCK | 0666 */
    st->st_blksize = 4096;
    return 0;
}

static uint16_t unix_vfs_poll(void *priv)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *us = unix_sock_get(id);
    if (!us) { spin_unlock_irqrestore(&unix_lock, fl); return 0; }

    uint16_t events = 0;

    switch (us->state) {
    case UNIX_LISTENING:
        if (us->accept_head != us->accept_tail)
            events |= 1;  /* POLLIN: pending connection */
        break;
    case UNIX_CONNECTED: {
        uint32_t peer_id = us->peer_id;
        unix_sock_t *peer = (peer_id != UNIX_NONE) ? unix_sock_get(peer_id) : (void *)0;
        if (peer && peer->ring_head != peer->ring_tail)
            events |= 1;  /* POLLIN: data in peer's tx ring */
        if (peer && ((uint16_t)(us->ring_head - us->ring_tail) < UNIX_BUF_SIZE))
            events |= 4;  /* POLLOUT: space in our tx ring */
        if (!peer || peer->state == UNIX_CLOSED)
            events |= 16; /* POLLHUP: peer disconnected */
        break;
    }
    case UNIX_CLOSED:
        events |= 16; /* POLLHUP */
        break;
    default:
        events |= 4;  /* POLLOUT: writable (not yet connected) */
        break;
    }

    spin_unlock_irqrestore(&unix_lock, fl);
    return events;
}

const vfs_ops_t g_unix_sock_ops = {
    .read    = unix_vfs_read,
    .write   = unix_vfs_write,
    .close   = unix_vfs_close,
    .readdir = (void *)0,
    .dup     = unix_vfs_dup,
    .stat    = unix_vfs_stat,
    .poll    = unix_vfs_poll,
};

/* ── Alloc / Get / Free ────────────────────────────────────────────────── */

int unix_sock_alloc(void)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    for (int i = 0; i < UNIX_SOCK_MAX; i++) {
        if (!s_unix[i].in_use) {
            /* Free orphaned ring from previous connection */
            if (s_unix[i].ring) {
                kva_free_pages(s_unix[i].ring, 1);
                s_unix[i].ring = (void *)0;
            }
            _memset(&s_unix[i], 0, sizeof(unix_sock_t));
            s_unix[i].in_use   = 1;
            s_unix[i].state    = UNIX_CREATED;
            s_unix[i].peer_id  = UNIX_NONE;
            s_unix[i].refcount = 1;
            spin_unlock_irqrestore(&unix_lock, fl);
            return i;
        }
    }
    spin_unlock_irqrestore(&unix_lock, fl);
    return -1;
}

unix_sock_t *unix_sock_get(uint32_t id)
{
    if (id >= UNIX_SOCK_MAX) return (void *)0;
    if (!s_unix[id].in_use) return (void *)0;
    return &s_unix[id];
}

void unix_sock_free(uint32_t id)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    if (id >= UNIX_SOCK_MAX || !s_unix[id].in_use) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return;
    }
    unix_sock_t *us = &s_unix[id];
    if (--us->refcount > 0) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return;
    }

    /* Wake peer — but do NOT clear peer's peer_id.  The peer needs
     * peer_id to locate our ring buffer for draining remaining data.
     * The peer detects closure via !s_unix[peer_id].in_use and returns
     * EOF after all buffered data has been read. */
    uint32_t peer = us->peer_id;
    if (peer != UNIX_NONE && peer < UNIX_SOCK_MAX && s_unix[peer].in_use) {
        if (s_unix[peer].waiter_task) {
            sched_wake(s_unix[peer].waiter_task);
            s_unix[peer].waiter_task = (void *)0;
        }
    }

    /* Close any staged fds that were never received */
    for (uint8_t i = 0; i < us->passed_fd_count; i++) {
        if (us->passed_fds[i].ops && us->passed_fds[i].ops->close)
            us->passed_fds[i].ops->close(us->passed_fds[i].priv);
    }

    /* Free ring buffer.  The peer reads from OUR ring, so we can only
     * free it when the peer is also gone.  If the peer is still alive,
     * leave our ring in place — the ring pointer stays valid even after
     * in_use=0 because the slot is not zeroed until reuse.  The peer
     * will free our ring when it closes (see below).
     *
     * If the peer is already gone, free both our ring AND the peer's
     * orphaned ring (the peer left its ring for us to read). */
    int peer_alive = (peer != UNIX_NONE && peer < UNIX_SOCK_MAX &&
                      s_unix[peer].in_use);
    if (!peer_alive) {
        /* Both sides closed — free both rings */
        if (us->ring) {
            kva_free_pages(us->ring, 1);
            us->ring = (void *)0;
        }
        if (peer != UNIX_NONE && peer < UNIX_SOCK_MAX && s_unix[peer].ring) {
            kva_free_pages(s_unix[peer].ring, 1);
            s_unix[peer].ring = (void *)0;
        }
    }
    /* else: peer alive — DON'T free our ring, peer still reads from it */

    /* Unregister name if bound */
    if (us->path[0])
        name_unregister(us->path);

    us->in_use = 0;
    spin_unlock_irqrestore(&unix_lock, fl);
}

void unix_sock_wake(uint32_t id)
{
    if (id >= UNIX_SOCK_MAX) return;
    unix_sock_t *us = &s_unix[id];
    if (us->waiter_task) {
        sched_wake(us->waiter_task);
        us->waiter_task = (void *)0;
    }
}

/* ── Bind ──────────────────────────────────────────────────────────────── */

int unix_sock_bind(uint32_t id, const char *path)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *us = unix_sock_get(id);
    if (!us || us->state != UNIX_CREATED) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -22;  /* EINVAL */
    }

    int rc = name_register(path, id);
    if (rc < 0) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return rc;
    }

    _strcpy(us->path, path, UNIX_PATH_MAX);
    us->state = UNIX_BOUND;
    spin_unlock_irqrestore(&unix_lock, fl);
    return 0;
}

/* ── Listen ────────────────────────────────────────────────────────────── */

int unix_sock_listen(uint32_t id)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *us = unix_sock_get(id);
    if (!us || us->state != UNIX_BOUND) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -22;  /* EINVAL */
    }
    us->state = UNIX_LISTENING;
    spin_unlock_irqrestore(&unix_lock, fl);
    return 0;
}

/* ── Connect ───────────────────────────────────────────────────────────── */

int unix_sock_connect(uint32_t id, const char *path)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *client = unix_sock_get(id);
    if (!client || client->state != UNIX_CREATED) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -22;  /* EINVAL */
    }

    /* Look up listening socket */
    uint32_t listener_id = name_lookup(path);
    if (listener_id == UNIX_NONE) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -111;  /* ECONNREFUSED */
    }
    unix_sock_t *listener = unix_sock_get(listener_id);
    if (!listener || listener->state != UNIX_LISTENING) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -111;  /* ECONNREFUSED */
    }

    /* Check accept queue capacity */
    uint8_t qlen = (uint8_t)((listener->accept_head - listener->accept_tail) & 7);
    if (qlen >= 8) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -111;  /* ECONNREFUSED: queue full */
    }

    /* Allocate server-side socket */
    int server_id = -1;
    for (int i = 0; i < UNIX_SOCK_MAX; i++) {
        if (!s_unix[i].in_use) {
            /* Free orphaned ring from previous connection if present */
            if (s_unix[i].ring) {
                kva_free_pages(s_unix[i].ring, 1);
                s_unix[i].ring = (void *)0;
            }
            _memset(&s_unix[i], 0, sizeof(unix_sock_t));
            s_unix[i].in_use   = 1;
            s_unix[i].state    = UNIX_CONNECTED;
            s_unix[i].peer_id  = id;  /* points to client */
            s_unix[i].refcount = 1;
            server_id = i;
            break;
        }
    }
    if (server_id < 0) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -24;  /* EMFILE */
    }

    /* Allocate ring buffers (one per direction) */
    spin_unlock_irqrestore(&unix_lock, fl);
    uint8_t *ring_a = (uint8_t *)kva_alloc_pages(1);  /* client→server */
    uint8_t *ring_b = (uint8_t *)kva_alloc_pages(1);  /* server→client */
    fl = spin_lock_irqsave(&unix_lock);

    /* Client's tx_ring = ring the server reads from */
    client->ring      = ring_a;
    client->ring_head = 0;
    client->ring_tail = 0;

    /* Server's tx_ring = ring the client reads from */
    s_unix[server_id].ring      = ring_b;
    s_unix[server_id].ring_head = 0;
    s_unix[server_id].ring_tail = 0;

    /* Cross-link */
    client->peer_id = (uint32_t)server_id;
    client->state   = UNIX_CONNECTED;

    /* Capture client credentials into server-side socket */
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    s_unix[server_id].peer_pid = (uint32_t)proc->pid;
    s_unix[server_id].peer_uid = proc->uid;
    s_unix[server_id].peer_gid = proc->gid;

    /* Capture server-side credentials into client socket
     * (will be the listener's process — available via accept caller).
     * For now set to 0, accept() will fill from accepting process. */
    client->peer_pid = 0;
    client->peer_uid = 0;
    client->peer_gid = 0;

    /* Enqueue server-side sock_id into listener's accept queue */
    listener->accept_queue[listener->accept_head & 7] = (uint32_t)server_id;
    listener->accept_head++;

    /* Wake listener if blocked in accept */
    if (listener->waiter_task) {
        sched_wake(listener->waiter_task);
        listener->waiter_task = (void *)0;
    }

    spin_unlock_irqrestore(&unix_lock, fl);
    return 0;
}

/* ── Accept ────────────────────────────────────────────────────────────── */

int unix_sock_accept(uint32_t id)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *listener = unix_sock_get(id);
    if (!listener || listener->state != UNIX_LISTENING) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -22;  /* EINVAL */
    }

    /* Block until accept queue non-empty */
    while (listener->accept_head == listener->accept_tail) {
        if (listener->nonblocking) {
            spin_unlock_irqrestore(&unix_lock, fl);
            return -11;  /* EAGAIN */
        }
        listener->waiter_task = sched_current();
        spin_unlock_irqrestore(&unix_lock, fl);
        sched_block();
        fl = spin_lock_irqsave(&unix_lock);
        listener = unix_sock_get(id);
        if (!listener || listener->state != UNIX_LISTENING) {
            spin_unlock_irqrestore(&unix_lock, fl);
            return -9;  /* EBADF */
        }
    }

    /* Dequeue */
    uint32_t server_id = listener->accept_queue[listener->accept_tail & 7];
    listener->accept_tail++;

    /* Fill peer credentials from accepting process */
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    unix_sock_t *server = unix_sock_get(server_id);
    if (server && server->peer_id != UNIX_NONE) {
        unix_sock_t *client = unix_sock_get(server->peer_id);
        if (client) {
            client->peer_pid = (uint32_t)proc->pid;
            client->peer_uid = proc->uid;
            client->peer_gid = proc->gid;
        }
    }

    spin_unlock_irqrestore(&unix_lock, fl);
    return (int)server_id;
}

/* ── Read (from peer's tx_ring) ────────────────────────────────────────── */

int unix_sock_read(uint32_t id, void *buf, uint32_t len)
{
    if (len == 0) return 0;
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *us = unix_sock_get(id);
    if (!us || us->state != UNIX_CONNECTED) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return us ? -32 : -9;  /* EPIPE or EBADF */
    }

    uint32_t peer = us->peer_id;

    for (;;) {
        /* Read from PEER's tx_ring.  Access the slot directly instead of
         * via unix_sock_get() — the peer may have closed (in_use=0) but
         * its ring buffer is kept alive until we close too. */
        unix_sock_t *p = (peer != UNIX_NONE && peer < UNIX_SOCK_MAX)
                         ? &s_unix[peer] : (void *)0;
        if (!p || !p->ring) {
            /* Peer gone AND ring freed — EOF */
            spin_unlock_irqrestore(&unix_lock, fl);
            return 0;
        }

        uint16_t avail = ring_used(p);
        if (avail > 0) {
            if (len > avail) len = avail;
            uint8_t *dst = (uint8_t *)buf;
            for (uint32_t i = 0; i < len; i++) {
                dst[i] = p->ring[p->ring_tail & (UNIX_BUF_SIZE - 1)];
                p->ring_tail++;
            }
            /* Wake peer if it was blocked on write (ring was full) */
            if (p->waiter_task) {
                sched_wake(p->waiter_task);
                p->waiter_task = (void *)0;
            }
            spin_unlock_irqrestore(&unix_lock, fl);
            return (int)len;
        }

        /* Empty — if peer closed, that's EOF (no more data coming) */
        if (!p->in_use) {
            spin_unlock_irqrestore(&unix_lock, fl);
            return 0;  /* EOF */
        }
        /* Empty — block */
        if (us->nonblocking) {
            spin_unlock_irqrestore(&unix_lock, fl);
            return -11;  /* EAGAIN */
        }
        us->waiter_task = sched_current();
        spin_unlock_irqrestore(&unix_lock, fl);
        sched_block();
        fl = spin_lock_irqsave(&unix_lock);
        us = unix_sock_get(id);
        if (!us || us->state != UNIX_CONNECTED) {
            spin_unlock_irqrestore(&unix_lock, fl);
            return 0;  /* EOF */
        }
        peer = us->peer_id;
    }
}

/* ── Write (to own tx_ring, peer reads from it) ────────────────────────── */

int unix_sock_write(uint32_t id, const void *buf, uint32_t len)
{
    if (len == 0) return 0;
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *us = unix_sock_get(id);
    if (!us || us->state != UNIX_CONNECTED || !us->ring) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -32;  /* EPIPE */
    }

    uint32_t peer = us->peer_id;
    if (peer == UNIX_NONE) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -32;  /* EPIPE */
    }

    uint32_t written = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (written < len) {
        uint16_t space = ring_free(us);
        if (space > 0) {
            uint32_t chunk = len - written;
            if (chunk > space) chunk = space;
            for (uint32_t i = 0; i < chunk; i++) {
                us->ring[us->ring_head & (UNIX_BUF_SIZE - 1)] = src[written + i];
                us->ring_head++;
            }
            written += chunk;

            /* Wake peer if blocked on read */
            unix_sock_t *p = unix_sock_get(peer);
            if (p && p->waiter_task) {
                sched_wake(p->waiter_task);
                p->waiter_task = (void *)0;
            }
        }

        if (written < len) {
            /* Ring full — block */
            if (us->nonblocking) break;
            us->waiter_task = sched_current();
            spin_unlock_irqrestore(&unix_lock, fl);
            sched_block();
            fl = spin_lock_irqsave(&unix_lock);
            us = unix_sock_get(id);
            if (!us || us->state != UNIX_CONNECTED || us->peer_id == UNIX_NONE) {
                spin_unlock_irqrestore(&unix_lock, fl);
                return written > 0 ? (int)written : -32;
            }
            peer = us->peer_id;
        }
    }

    spin_unlock_irqrestore(&unix_lock, fl);
    return (int)written;
}

/* ── Peer credentials ──────────────────────────────────────────────────── */

int unix_sock_peercred(uint32_t id, uint32_t *pid, uint32_t *uid, uint32_t *gid)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *us = unix_sock_get(id);
    if (!us || us->state != UNIX_CONNECTED) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -107;  /* ENOTCONN */
    }
    *pid = us->peer_pid;
    *uid = us->peer_uid;
    *gid = us->peer_gid;
    spin_unlock_irqrestore(&unix_lock, fl);
    return 0;
}

/* ── fd passing (SCM_RIGHTS) ───────────────────────────────────────────── */

int unix_sock_stage_fds(uint32_t peer_id, unix_passed_fd_t *fds, uint8_t count)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *p = unix_sock_get(peer_id);
    if (!p) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -9;  /* EBADF */
    }
    if (p->passed_fd_count + count > UNIX_PASSED_FD_MAX) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return -105;  /* ENOBUFS */
    }
    for (uint8_t i = 0; i < count; i++)
        p->passed_fds[p->passed_fd_count++] = fds[i];
    spin_unlock_irqrestore(&unix_lock, fl);
    return 0;
}

int unix_sock_recv_fds(uint32_t id, int *fd_out, int max_fds)
{
    irqflags_t fl = spin_lock_irqsave(&unix_lock);
    unix_sock_t *us = unix_sock_get(id);
    if (!us || us->passed_fd_count == 0) {
        spin_unlock_irqrestore(&unix_lock, fl);
        return 0;
    }

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    int installed = 0;

    for (uint8_t i = 0; i < us->passed_fd_count && installed < max_fds; i++) {
        /* Find free fd slot */
        int free_fd = -1;
        for (int f = 0; f < PROC_MAX_FDS; f++) {
            if (!proc->fd_table->fds[f].ops) { free_fd = f; break; }
        }
        if (free_fd < 0) break;  /* fd table full */

        proc->fd_table->fds[free_fd].ops    = us->passed_fds[i].ops;
        proc->fd_table->fds[free_fd].priv   = us->passed_fds[i].priv;
        proc->fd_table->fds[free_fd].offset = 0;
        proc->fd_table->fds[free_fd].size   = 0;
        proc->fd_table->fds[free_fd].flags  = us->passed_fds[i].flags;
        fd_out[installed++] = free_fd;
    }

    /* Clear staging area (drop any fds that couldn't be installed) */
    for (uint8_t i = (uint8_t)installed; i < us->passed_fd_count; i++) {
        if (us->passed_fds[i].ops && us->passed_fds[i].ops->close)
            us->passed_fds[i].ops->close(us->passed_fds[i].priv);
    }
    us->passed_fd_count = 0;

    spin_unlock_irqrestore(&unix_lock, fl);
    return installed;
}

/* ── fd helpers ────────────────────────────────────────────────────────── */

int unix_sock_open_fd(uint32_t sock_id, void *proc_ptr)
{
    aegis_process_t *proc = (aegis_process_t *)proc_ptr;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!proc->fd_table->fds[i].ops) {
            proc->fd_table->fds[i].ops    = &g_unix_sock_ops;
            proc->fd_table->fds[i].priv   = (void *)(uintptr_t)sock_id;
            proc->fd_table->fds[i].offset = 0;
            proc->fd_table->fds[i].size   = 0;
            proc->fd_table->fds[i].flags  = 0;
            return i;
        }
    }
    return -1;
}

uint32_t unix_sock_id_from_fd(int fd, void *proc_ptr)
{
    aegis_process_t *proc = (aegis_process_t *)proc_ptr;
    if (fd < 0 || fd >= PROC_MAX_FDS) return UNIX_NONE;
    if (proc->fd_table->fds[fd].ops != &g_unix_sock_ops) return UNIX_NONE;
    return (uint32_t)(uintptr_t)proc->fd_table->fds[fd].priv;
}
