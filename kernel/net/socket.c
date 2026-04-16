/* kernel/net/socket.c — socket table */
#include "socket.h"
#include "proc.h"
#include "vfs.h"
#include "printk.h"
#include "tcp.h"
#include "udp.h"
#include "sched.h"
#include "uaccess.h"
#include "spinlock.h"
#include <stdint.h>
#include <stddef.h>

static sock_t s_socks[SOCK_TABLE_SIZE];  /* zero-initialized by C runtime */
static spinlock_t sock_lock = SPINLOCK_INIT;

/* ── Socket VFS ops ─────────────────────────────────────────────────────── */

static int  sock_vfs_read(void *priv, void *buf, uint64_t off, uint64_t len);
static int  sock_vfs_write(void *priv, const void *buf, uint64_t len);
static void sock_vfs_close(void *priv);
static void sock_vfs_dup(void *priv);
static int  sock_vfs_stat(void *priv, k_stat_t *st);

static const vfs_ops_t s_sock_ops = {
    .read    = sock_vfs_read,
    .write   = sock_vfs_write,
    .close   = sock_vfs_close,
    .readdir = (void *)0,
    .dup     = sock_vfs_dup,
    .stat    = sock_vfs_stat,
    .poll    = (void *)0,
};

static int sock_vfs_read(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)off;
    uint32_t sock_id = (uint32_t)(uintptr_t)priv;
    sock_t *s = sock_get(sock_id);
    if (!s) return -9;  /* EBADF */

    if (s->type == SOCK_TYPE_STREAM) {
        /* TCP: blocking recv.  Returns byte count, 0=EOF, -EPIPE on close.
         *
         * avail > 0  → data available: read it.
         * avail == 0 → no data in rbuf; must check TCP state:
         *   ESTABLISHED/SYN_RCVD: block (or EAGAIN if nonblocking).
         *   CLOSE_WAIT/CLOSED:    return 0 (EOF — FIN received).
         */
        for (;;) {
            /* Set waiter BEFORE checking data to prevent lost wakeup:
             * if sock_wake fires between the peek and sched_block while
             * waiter_task is NULL, the wakeup would be silently lost. */
            s->waiter_task = (aegis_task_t *)sched_current();
            int avail = tcp_conn_recv(s->tcp_conn_id, (void *)0, 0);  /* peek */
            if (avail > 0) {
                s->waiter_task = (aegis_task_t *)0;  /* clear — not blocking */
                uint32_t want = (uint32_t)len < (uint32_t)avail ? (uint32_t)len : (uint32_t)avail;
                if (want > 8192) want = 8192;
                return tcp_conn_recv(s->tcp_conn_id, buf, (uint16_t)want);
            }
            /* avail == 0: check if EOF or just no data yet */
            {
                tcp_conn_t *tc = tcp_conn_get(s->tcp_conn_id);
                if (!tc || tc->state == TCP_CLOSE_WAIT || tc->state == TCP_CLOSED
                    || tc->state == TCP_TIME_WAIT) {
                    s->waiter_task = (aegis_task_t *)0;
                    return 0;  /* EOF — FIN received */
                }
            }
            if (s->nonblocking) {
                s->waiter_task = (aegis_task_t *)0;
                return -11;  /* EAGAIN */
            }
            sched_block();
        }
    }
    /* UDP: peek from ring buffer — kernel buf already filled via recvfrom */
    return -38;  /* ENOSYS for UDP via read() — use recvfrom */
}

static int sock_vfs_write(void *priv, const void *buf, uint64_t len)
{
    uint32_t sock_id = (uint32_t)(uintptr_t)priv;
    sock_t *s = sock_get(sock_id);
    if (!s) return -9;  /* EBADF */

    if (s->type == SOCK_TYPE_STREAM) {
        if (s->state != SOCK_CONNECTED) return -107;  /* ENOTCONN */
        /* buf is a raw user-space pointer from sys_write.  Copy to kernel
         * staging before passing to tcp_conn_send to avoid SMAP fault. */
        uint8_t s_sndbuf[1460];
        uint32_t sent = 0;
        while (sent < (uint32_t)len) {
            uint32_t chunk = (uint32_t)len - sent;
            if (chunk > 1460) chunk = 1460;
            copy_from_user(s_sndbuf, (const uint8_t *)buf + sent, chunk);
            int n = tcp_conn_send(s->tcp_conn_id, s_sndbuf, (uint16_t)chunk);
            if (n <= 0) return sent > 0 ? (int)sent : -32;  /* EPIPE */
            sent += (uint32_t)n;
        }
        return (int)sent;
    }
    return -38;  /* ENOSYS for UDP via write() — use sendto */
}

static void sock_vfs_close(void *priv)
{
    uint32_t sock_id = (uint32_t)(uintptr_t)priv;
    /* Release the UDP port binding (if any) before freeing the slot.
     * Without this, every closed UDP socket leaks its port forever and
     * the next bind() to the same port returns EADDRINUSE. The DHCP
     * client retry loop was the symptom that caught this. */
    sock_t *s = sock_get(sock_id);
    if (s && s->type == SOCK_TYPE_DGRAM && s->local_port != 0)
        udp_unbind(s->local_port);
    sock_free(sock_id);
}

static void sock_vfs_dup(void *priv)
{
    (void)priv;  /* sockets have no refcount — each fd is independent */
}

static int sock_vfs_stat(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode = 0140666;  /* S_IFSOCK | 0666 */
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int sock_alloc(uint8_t type)
{
    irqflags_t fl = spin_lock_irqsave(&sock_lock);
    uint32_t i;
    for (i = 0; i < SOCK_TABLE_SIZE; i++) {
        if (s_socks[i].state == SOCK_FREE) {
            __builtin_memset(&s_socks[i], 0, sizeof(s_socks[i]));
            s_socks[i].state       = SOCK_CREATED;
            s_socks[i].type        = type;
            s_socks[i].tcp_conn_id = SOCK_NONE;
            s_socks[i].epoll_id    = SOCK_NONE;
            waitq_init(&s_socks[i].poll_waiters);
            spin_unlock_irqrestore(&sock_lock, fl);
            return (int)i;
        }
    }
    spin_unlock_irqrestore(&sock_lock, fl);
    return -1;
}

sock_t *sock_get(uint32_t sock_id)
{
    irqflags_t fl = spin_lock_irqsave(&sock_lock);
    if (sock_id >= SOCK_TABLE_SIZE) {
        spin_unlock_irqrestore(&sock_lock, fl);
        return (sock_t *)0;
    }
    if (s_socks[sock_id].state == SOCK_FREE) {
        spin_unlock_irqrestore(&sock_lock, fl);
        return (sock_t *)0;
    }
    sock_t *s = &s_socks[sock_id];
    spin_unlock_irqrestore(&sock_lock, fl);
    return s;
}

/* sock_get_nolock: return pointer without acquiring sock_lock.
 * Only safe when caller holds a lock that prevents concurrent sock_free
 * (e.g. tcp_lock or udp_lock — the binding/conn table references keep
 * the socket alive).  Used to avoid lock ordering inversions. */
sock_t *sock_get_nolock(uint32_t sock_id)
{
    if (sock_id >= SOCK_TABLE_SIZE)
        return (sock_t *)0;
    if (s_socks[sock_id].state == SOCK_FREE)
        return (sock_t *)0;
    return &s_socks[sock_id];
}

void sock_free(uint32_t sock_id)
{
    irqflags_t fl = spin_lock_irqsave(&sock_lock);
    if (sock_id >= SOCK_TABLE_SIZE) {
        spin_unlock_irqrestore(&sock_lock, fl);
        return;
    }
    s_socks[sock_id].state = SOCK_FREE;
    s_socks[sock_id].waiter_task = (aegis_task_t *)0;
    spin_unlock_irqrestore(&sock_lock, fl);
}

void sock_wake(uint32_t sock_id)
{
    sock_t *s = sock_get(sock_id);
    if (!s) return;
    if (s->waiter_task) {
        sched_wake(s->waiter_task);
        s->waiter_task = (aegis_task_t *)0;
    }
}

int sock_open_fd(uint32_t sock_id, aegis_process_t *proc)
{
    uint32_t fd;
    for (fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (!proc->fd_table->fds[fd].ops) {
            proc->fd_table->fds[fd].ops    = &s_sock_ops;
            proc->fd_table->fds[fd].priv   = (void *)(uintptr_t)sock_id;
            proc->fd_table->fds[fd].offset = 0;
            proc->fd_table->fds[fd].size   = 0;
            proc->fd_table->fds[fd].flags  = 0;
            proc->fd_table->fds[fd]._pad   = 0;
            return (int)fd;
        }
    }
    return -1;  /* EMFILE */
}

uint32_t sock_id_from_fd(int fd, aegis_process_t *proc)
{
    if (fd < 0 || (uint32_t)fd >= PROC_MAX_FDS) return SOCK_NONE;
    if (!proc->fd_table->fds[fd].ops) return SOCK_NONE;
    if (proc->fd_table->fds[fd].ops != &s_sock_ops) return SOCK_NONE;
    return (uint32_t)(uintptr_t)proc->fd_table->fds[fd].priv;
}

/* sock_get_waitq: return the embedded poll_waiters waitq for sock_id, or
 * NULL if the slot is free/invalid. fd_waitq dispatches sys_poll and
 * sys_epoll_wait waiters here. Producers in tcp.c / udp.c / socket.c
 * call waitq_wake_all(&s->poll_waiters) on rx, accept enqueue, and
 * TCP state→CLOSE_WAIT/CLOSED/TIME_WAIT. */
waitq_t *
sock_get_waitq(uint32_t id)
{
    sock_t *s = sock_get(id);
    return s ? &s->poll_waiters : NULL;
}
