/* sys_socket.c — POSIX socket API syscalls */
#include "sys_impl.h"
#include "sched.h"
#include "waitq.h"
#include "proc.h"
#include "vfs.h"
#include "socket.h"
#include "epoll.h"
#include "arch.h"
#include "netdev.h"
#include "eth.h"
#include "udp.h"
#include "tcp.h"
#include "ip.h"
#include "unix_socket.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define SOCK_STREAM    1
#define SOCK_DGRAM     2

#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_BROADCAST   6
#define SO_PEERCRED    17
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21

#define IPPROTO_TCP    6
#define TCP_NODELAY    1

/* sockaddr_un layout */
typedef struct {
    uint16_t sun_family;
    char     sun_path[UNIX_PATH_MAX];
} k_sockaddr_un_t;


/* net_get_config / net_set_config declared in ip.h (already included) */

/* ── Helpers ────────────────────────────────────────────────────────────── */

static uint64_t
get_proc_sock(uint64_t fd_arg, sock_t **s_out, uint32_t *sid_out)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t sid = sock_id_from_fd((int)fd_arg, proc);
    if (sid == SOCK_NONE) return (uint64_t)-(int64_t)9;  /* EBADF */
    sock_t *s = sock_get(sid);
    if (!s) return (uint64_t)-(int64_t)9;
    *s_out   = s;
    *sid_out = sid;
    return 0;
}

/* ── sys_socket ────────────────────────────────────────────────────────── */

uint64_t
sys_socket(uint64_t domain, uint64_t type, uint64_t proto)
{
    (void)proto;

    /* AF_UNIX path */
    if (domain == AF_UNIX) {
        if (type != SOCK_STREAM) return (uint64_t)-(int64_t)93;
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        if (cap_check(proc->caps, CAP_TABLE_SIZE, CAP_KIND_IPC, CAP_RIGHTS_READ) != 0)
            return (uint64_t)-(int64_t)130;  /* ENOCAP */
        int uid = unix_sock_alloc();
        if (uid < 0) return (uint64_t)-(int64_t)24;
        int fd = unix_sock_open_fd((uint32_t)uid, proc);
        if (fd < 0) { unix_sock_free((uint32_t)uid); return (uint64_t)-(int64_t)24; }
        return (uint64_t)fd;
    }

    /* AF_INET path */
    if (domain != AF_INET) return (uint64_t)-(int64_t)97;  /* EAFNOSUPPORT */
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return (uint64_t)-(int64_t)93;  /* EPROTONOSUPPORT */

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE, CAP_KIND_NET_SOCKET, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)130;  /* ENOCAP */

    int sid = sock_alloc((uint8_t)(type == SOCK_STREAM ? SOCK_TYPE_STREAM : SOCK_TYPE_DGRAM));
    if (sid < 0) return (uint64_t)-(int64_t)24;  /* EMFILE */

    int fd = sock_open_fd((uint32_t)sid, proc);
    if (fd < 0) { sock_free((uint32_t)sid); return (uint64_t)-(int64_t)24; }

    return (uint64_t)fd;
}

/* ── sys_bind ──────────────────────────────────────────────────────────── */

uint64_t
sys_bind(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* AF_UNIX bind */
    uint32_t uid = unix_sock_id_from_fd((int)fd, proc);
    if (uid != UNIX_NONE) {
        if (addrlen < 4 || !user_ptr_valid(addr, addrlen))
            return (uint64_t)-(int64_t)14;
        k_sockaddr_un_t sun;
        __builtin_memset(&sun, 0, sizeof(sun));
        uint32_t copy_len = addrlen < sizeof(sun) ? (uint32_t)addrlen : (uint32_t)sizeof(sun);
        copy_from_user(&sun, (const void *)(uintptr_t)addr, copy_len);
        if (sun.sun_family != AF_UNIX) return (uint64_t)-(int64_t)22;
        sun.sun_path[UNIX_PATH_MAX - 1] = '\0';
        int rc = unix_sock_bind(uid, sun.sun_path);
        return rc < 0 ? (uint64_t)(int64_t)rc : 0;
    }

    /* AF_INET bind */
    if (addrlen < sizeof(k_sockaddr_in_t)) return (uint64_t)-(int64_t)22;
    if (!user_ptr_valid(addr, sizeof(k_sockaddr_in_t))) return (uint64_t)-(int64_t)14;

    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;

    k_sockaddr_in_t sa;
    copy_from_user(&sa, (const void *)(uintptr_t)addr, sizeof(sa));
    if (sa.sin_family != AF_INET) return (uint64_t)-(int64_t)22;

    uint16_t port = ntohs(sa.sin_port);
    if (port == 0) return (uint64_t)-(int64_t)22;  /* EINVAL: no wildcard port */

    s->local_ip   = sa.sin_addr;
    s->local_port = port;
    s->state      = SOCK_BOUND;

    /* Register with TCP or UDP binding layer */
    if (s->type == SOCK_TYPE_DGRAM) {
        return (uint64_t)(udp_bind(port, sid) == 0 ? 0 : -(int64_t)98); /* EADDRINUSE */
    }
    /* TCP: binding is stored in sock_t; actual listen registration happens in sys_listen */
    return 0;
}

/* ── sys_listen ────────────────────────────────────────────────────────── */

uint64_t
sys_listen(uint64_t fd, uint64_t backlog)
{
    (void)backlog;
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t uid = unix_sock_id_from_fd((int)fd, proc);
    if (uid != UNIX_NONE) {
        int rc = unix_sock_listen(uid);
        return rc < 0 ? (uint64_t)(int64_t)rc : 0;
    }

    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;
    if (s->type != SOCK_TYPE_STREAM) return (uint64_t)-(int64_t)95;  /* EOPNOTSUPP */
    if (s->state < SOCK_BOUND) return (uint64_t)-(int64_t)22;  /* EINVAL: must bind first */

    s->state = SOCK_LISTENING;

    return (uint64_t)(tcp_listen(s->local_port, sid) == 0 ? 0 : -(int64_t)98);
}

/* ── sys_accept ────────────────────────────────────────────────────────── */

uint64_t
sys_accept(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    aegis_process_t *proc_a = (aegis_process_t *)sched_current();
    uint32_t uid = unix_sock_id_from_fd((int)fd, proc_a);
    if (uid != UNIX_NONE) {
        int server_id = unix_sock_accept(uid);
        if (server_id < 0) return (uint64_t)(int64_t)server_id;
        int new_fd = unix_sock_open_fd((uint32_t)server_id, proc_a);
        if (new_fd < 0) { unix_sock_free((uint32_t)server_id); return (uint64_t)-(int64_t)24; }
        return (uint64_t)new_fd;
    }

    sock_t *ls; uint32_t lsid;
    uint64_t err = get_proc_sock(fd, &ls, &lsid);
    if (err) return err;
    if (ls->state != SOCK_LISTENING) return (uint64_t)-(int64_t)22;

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    for (;;) {
        /* Set waiter_task BEFORE checking the queue to prevent lost wakeups.
         * If sock_wake fires between our check and sched_block, it sets
         * state=RUNNING, so sched_block becomes a no-op (task stays runnable).
         * Without this, sock_wake could fire while waiter_task is NULL and
         * the wakeup would be silently lost — accept() blocks forever. */
        ls->waiter_task = (aegis_task_t *)sched_current();

        /* Check accept queue */
        if (ls->accept_head != ls->accept_tail) {
            ls->waiter_task = (aegis_task_t *)0;
            uint32_t conn_id = ls->accept_queue[ls->accept_head];
            ls->accept_head = (ls->accept_head + 1) & 7;

            /* Allocate a new sock_t for this connected peer */
            int new_sid = sock_alloc(SOCK_TYPE_STREAM);
            if (new_sid < 0) return (uint64_t)-(int64_t)12;
            sock_t *ns = sock_get((uint32_t)new_sid);
            ns->state       = SOCK_CONNECTED;
            ns->tcp_conn_id = conn_id;

            /* Copy peer address from tcp_conn_t */
            tcp_conn_get_addr(conn_id, &ns->remote_ip, &ns->remote_port,
                              &ns->local_ip, &ns->local_port);

            /* Point tcp_conn back to this new socket */
            tcp_conn_set_sock(conn_id, (uint32_t)new_sid);

            /* Fill caller's addr struct */
            if (addr && addrlen) {
                if (!user_ptr_valid(addr, sizeof(k_sockaddr_in_t))) return (uint64_t)-(int64_t)14;  /* EFAULT */
                if (!user_ptr_valid(addrlen, sizeof(uint32_t))) return (uint64_t)-(int64_t)14;  /* EFAULT */
                k_sockaddr_in_t sa;
                sa.sin_family = AF_INET;
                sa.sin_port   = htons(ns->remote_port);
                sa.sin_addr   = ns->remote_ip;
                __builtin_memset(sa.sin_zero, 0, 8);
                uint32_t outlen = sizeof(sa);
                copy_to_user((void *)(uintptr_t)addrlen, &outlen, sizeof(uint32_t));
                copy_to_user((void *)(uintptr_t)addr, &sa, sizeof(sa));
            }

            int new_fd = sock_open_fd((uint32_t)new_sid, proc);
            if (new_fd < 0) { sock_free((uint32_t)new_sid); return (uint64_t)-(int64_t)12; }
            return (uint64_t)new_fd;
        }

        if (ls->nonblocking) {
            ls->waiter_task = (aegis_task_t *)0;
            return (uint64_t)-(int64_t)11;  /* EAGAIN */
        }

        sched_block();
    }
}

/* ── sys_connect ───────────────────────────────────────────────────────── */

uint64_t
sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    aegis_process_t *proc_c = (aegis_process_t *)sched_current();
    uint32_t uid = unix_sock_id_from_fd((int)fd, proc_c);
    if (uid != UNIX_NONE) {
        if (addrlen < 4 || !user_ptr_valid(addr, addrlen))
            return (uint64_t)-(int64_t)14;
        k_sockaddr_un_t sun;
        __builtin_memset(&sun, 0, sizeof(sun));
        uint32_t copy_len = addrlen < sizeof(sun) ? (uint32_t)addrlen : (uint32_t)sizeof(sun);
        copy_from_user(&sun, (const void *)(uintptr_t)addr, copy_len);
        sun.sun_path[UNIX_PATH_MAX - 1] = '\0';
        int rc = unix_sock_connect(uid, sun.sun_path);
        return rc < 0 ? (uint64_t)(int64_t)rc : 0;
    }

    if (addrlen < sizeof(k_sockaddr_in_t)) return (uint64_t)-(int64_t)22;
    if (!user_ptr_valid(addr, sizeof(k_sockaddr_in_t))) return (uint64_t)-(int64_t)14;

    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;

    k_sockaddr_in_t sa;
    copy_from_user(&sa, (const void *)(uintptr_t)addr, sizeof(sa));

    s->remote_ip   = sa.sin_addr;
    s->remote_port = ntohs(sa.sin_port);

    if (s->type == SOCK_TYPE_DGRAM) {
        /* UDP connect: just set remote addr. No handshake. */
        s->state = SOCK_CONNECTED;
        return 0;
    }

    /* TCP: initiate SYN.  Set waiter_task BEFORE sending SYN so that
     * if the ACK arrives in the same PIT tick, sock_wake finds us. */
    s->waiter_task = (aegis_task_t *)sched_current();

    uint32_t conn_id;
    int r = tcp_connect(sid, sa.sin_addr, ntohs(sa.sin_port), &conn_id);
    if (r < 0) {
        s->waiter_task = (aegis_task_t *)0;
        return (uint64_t)(int64_t)r;
    }
    s->tcp_conn_id = conn_id;
    s->state       = SOCK_CONNECTING;

    /* Block until ESTABLISHED */
    sched_block();
    /* After wake: check if connected */
    if (s->state != SOCK_CONNECTED) return (uint64_t)-(int64_t)111;  /* ECONNREFUSED */
    return 0;
}

/* ── sys_sendto ────────────────────────────────────────────────────────── */

uint64_t
sys_sendto(uint64_t fd, uint64_t buf, uint64_t len,
           uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    (void)flags;
    if (!user_ptr_valid(buf, len)) return (uint64_t)-(int64_t)14;

    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;

    if (s->type == SOCK_TYPE_STREAM) {
        /* TCP send */
        if (s->state != SOCK_CONNECTED) return (uint64_t)-(int64_t)107;  /* ENOTCONN */
        /* Copy from userspace to a kernel bounce buffer (max 1460 per call) */
        uint8_t sndbuf[1460];
        uint64_t sent = 0;
        while (sent < len) {
            uint64_t chunk = len - sent;
            if (chunk > 1460) chunk = 1460;
            copy_from_user(sndbuf, (const void *)(uintptr_t)(buf + sent), (uint32_t)chunk);
            int n = tcp_conn_send(s->tcp_conn_id, sndbuf, (uint16_t)chunk);
            if (n <= 0) return sent > 0 ? (uint64_t)sent : (uint64_t)-(int64_t)32; /* EPIPE */
            sent += (uint64_t)n;
        }
        return sent;
    }

    /* UDP send */
    ip4_addr_t dst_ip;
    uint16_t   dst_port;
    if (addr && addrlen >= sizeof(k_sockaddr_in_t)) {
        k_sockaddr_in_t sa;
        copy_from_user(&sa, (const void *)(uintptr_t)addr, sizeof(sa));
        dst_ip   = sa.sin_addr;
        dst_port = ntohs(sa.sin_port);
    } else if (s->state == SOCK_CONNECTED) {
        dst_ip   = s->remote_ip;
        dst_port = s->remote_port;
    } else {
        return (uint64_t)-(int64_t)89;  /* EDESTADDRREQ */
    }

    if (len > 1472) len = 1472;  /* max UDP payload for 1500-byte MTU */
    static uint8_t s_udpbuf[1472];
    copy_from_user(s_udpbuf, (const void *)(uintptr_t)buf, (uint32_t)len);

    netdev_t *dev = netdev_get("eth0");
    if (!dev) return (uint64_t)-(int64_t)100;  /* ENETDOWN */

    int r = udp_send(dev, s->local_port, dst_ip, dst_port, s_udpbuf, (uint16_t)len);
    return r < 0 ? (uint64_t)-(int64_t)100 : len;
}

/* ── sys_recvfrom ──────────────────────────────────────────────────────── */

uint64_t
sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len,
             uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    (void)flags;
    if (!user_ptr_valid(buf, len)) return (uint64_t)-(int64_t)14;

    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;

    uint32_t deadline = 0;
    uint8_t  has_timeout = 0;
    if (s->rcvtimeo_ticks > 0) {
        deadline = (uint32_t)arch_get_ticks() + s->rcvtimeo_ticks;
        has_timeout = 1;
    }

    if (s->type == SOCK_TYPE_STREAM) {
        /* TCP recv — set waiter_task before checking data to prevent
         * lost wakeups (same pattern as accept fix). */
        for (;;) {
            s->waiter_task = (aegis_task_t *)sched_current();
            int n = tcp_conn_recv(s->tcp_conn_id, (void *)0, 0);
            if (n > 0) {
                s->waiter_task = (aegis_task_t *)0;
                static uint8_t s_rcvbuf[8192];
                uint32_t want = (uint32_t)len < (uint32_t)n ? (uint32_t)len : (uint32_t)n;
                if (want > 8192) want = 8192;
                n = tcp_conn_recv(s->tcp_conn_id, s_rcvbuf, (uint16_t)want);
                if (n > 0) {
                    copy_to_user((void *)(uintptr_t)buf, s_rcvbuf, (uint32_t)n);
                    return (uint64_t)n;
                }
            }
            s->waiter_task = (aegis_task_t *)0;
            if (n == 0) return 0;  /* EOF / FIN */
            if (s->nonblocking) return (uint64_t)-(int64_t)11;  /* EAGAIN */
            if (has_timeout && (uint32_t)arch_get_ticks() >= deadline)
                return (uint64_t)-(int64_t)110;  /* ETIMEDOUT */
            s->waiter_task = (aegis_task_t *)sched_current();
            /* If a timeout is set, arm the scheduler's sleep_deadline so
             * sched_tick auto-wakes us at the deadline even if no data
             * arrives. Without this, sched_block() waits forever. */
            if (has_timeout)
                sched_current()->sleep_deadline = deadline;
            sched_block();
            if (has_timeout)
                sched_current()->sleep_deadline = 0;
        }
    }

    /* UDP recv — set waiter_task before checking data to prevent
     * lost wakeups. */
    for (;;) {
        s->waiter_task = (aegis_task_t *)sched_current();
        if (s->udp_rx_head != s->udp_rx_tail) {
            s->waiter_task = (aegis_task_t *)0;
            udp_rx_slot_t *slot = &s->udp_rx[s->udp_rx_head];
            s->udp_rx_head = (s->udp_rx_head + 1) & (UDP_RX_SLOTS - 1);
            uint32_t copy_len = slot->len < (uint32_t)len ? slot->len : (uint32_t)len;
            copy_to_user((void *)(uintptr_t)buf, slot->data, copy_len);
            if (addr && addrlen &&
                user_ptr_valid(addr, sizeof(k_sockaddr_in_t)) &&
                user_ptr_valid(addrlen, sizeof(uint32_t))) {
                k_sockaddr_in_t sa;
                sa.sin_family = AF_INET;
                sa.sin_port   = htons(slot->src_port);
                sa.sin_addr   = slot->src_ip;
                __builtin_memset(sa.sin_zero, 0, 8);
                uint32_t outlen = sizeof(sa);
                copy_to_user((void *)(uintptr_t)addrlen, &outlen, sizeof(uint32_t));
                copy_to_user((void *)(uintptr_t)addr, &sa, sizeof(sa));
            }
            slot->in_use = 0;
            return (uint64_t)copy_len;
        }
        s->waiter_task = (aegis_task_t *)0;
        if (s->nonblocking) return (uint64_t)-(int64_t)11;  /* EAGAIN */
        if (has_timeout && (uint32_t)arch_get_ticks() >= deadline)
            return (uint64_t)-(int64_t)110;  /* ETIMEDOUT */
        s->waiter_task = (aegis_task_t *)sched_current();
        /* Same sleep_deadline trick as the TCP path above — without it
         * sched_block() waits forever and the deadline check never fires. */
        if (has_timeout)
            sched_current()->sleep_deadline = deadline;
        sched_block();
        if (has_timeout)
            sched_current()->sleep_deadline = 0;
    }
}

/* ── sys_sendmsg / sys_recvmsg — AF_UNIX with SCM_RIGHTS ──────────────── */

/* Linux ABI structs for sendmsg/recvmsg */
typedef struct { void *iov_base; uint64_t iov_len; } k_iovec_t;
typedef struct {
    void      *msg_name;
    uint32_t   msg_namelen;
    uint32_t   _pad0;
    k_iovec_t *msg_iov;
    uint64_t   msg_iovlen;
    void      *msg_control;
    uint64_t   msg_controllen;
    int        msg_flags;
} k_msghdr_t;
typedef struct {
    uint64_t cmsg_len;
    int      cmsg_level;
    int      cmsg_type;
    /* payload follows */
} k_cmsghdr_t;

#define SCM_RIGHTS 1

uint64_t sys_sendmsg(uint64_t fd, uint64_t msg_ptr, uint64_t flags)
{
    (void)flags;
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t uid = unix_sock_id_from_fd((int)fd, proc);
    if (uid == UNIX_NONE) return (uint64_t)-(int64_t)38;  /* ENOSYS for non-unix */

    if (!user_ptr_valid(msg_ptr, sizeof(k_msghdr_t))) return (uint64_t)-(int64_t)14;
    k_msghdr_t mh;
    copy_from_user(&mh, (const void *)(uintptr_t)msg_ptr, sizeof(mh));

    /* Send iov data */
    int64_t total_sent = 0;
    for (uint64_t i = 0; i < mh.msg_iovlen && i < 8; i++) {
        k_iovec_t iov;
        if (!user_ptr_valid((uint64_t)(uintptr_t)mh.msg_iov + i * sizeof(k_iovec_t), sizeof(k_iovec_t)))
            return (uint64_t)-(int64_t)14;
        copy_from_user(&iov, (const void *)((uintptr_t)mh.msg_iov + i * sizeof(k_iovec_t)), sizeof(iov));
        if (iov.iov_len == 0) continue;
        if (!user_ptr_valid((uint64_t)(uintptr_t)iov.iov_base, iov.iov_len))
            return (uint64_t)-(int64_t)14;
        /* Copy iov data to kernel buffer, then write to socket */
        uint8_t kbuf[1024];
        uint64_t remain = iov.iov_len;
        uint64_t off = 0;
        while (remain > 0) {
            uint32_t chunk = remain > 1024 ? 1024 : (uint32_t)remain;
            copy_from_user(kbuf, (const void *)((uintptr_t)iov.iov_base + off), chunk);
            int n = unix_sock_write(uid, kbuf, chunk);
            if (n < 0) return total_sent > 0 ? (uint64_t)total_sent : (uint64_t)(int64_t)n;
            total_sent += n;
            off += (uint64_t)n;
            remain -= (uint64_t)n;
        }
    }

    /* Process ancillary data (SCM_RIGHTS) */
    if (mh.msg_control && mh.msg_controllen >= sizeof(k_cmsghdr_t)) {
        if (!user_ptr_valid((uint64_t)(uintptr_t)mh.msg_control, mh.msg_controllen))
            return (uint64_t)-(int64_t)14;
        k_cmsghdr_t cm;
        copy_from_user(&cm, (const void *)(uintptr_t)mh.msg_control, sizeof(cm));

        if (cm.cmsg_level == SOL_SOCKET && cm.cmsg_type == SCM_RIGHTS) {
            uint64_t payload_len = cm.cmsg_len - sizeof(k_cmsghdr_t);
            int nfds = (int)(payload_len / sizeof(int));
            if (nfds > UNIX_PASSED_FD_MAX) nfds = UNIX_PASSED_FD_MAX;

            int sender_fds[UNIX_PASSED_FD_MAX];
            copy_from_user(sender_fds,
                (const void *)((uintptr_t)mh.msg_control + sizeof(k_cmsghdr_t)),
                (uint32_t)(nfds * sizeof(int)));

            /* Dup each fd and stage for peer */
            unix_sock_t *us = unix_sock_get(uid);
            if (us && us->peer_id != UNIX_NONE) {
                unix_passed_fd_t staged[UNIX_PASSED_FD_MAX];
                uint8_t count = 0;
                for (int i = 0; i < nfds; i++) {
                    int sfd = sender_fds[i];
                    if (sfd < 0 || sfd >= PROC_MAX_FDS) continue;
                    vfs_file_t *f = &proc->fd_table->fds[sfd];
                    if (!f->ops) continue;
                    if (f->ops->dup) f->ops->dup(f->priv);
                    staged[count].ops   = f->ops;
                    staged[count].priv  = f->priv;
                    staged[count].flags = f->flags;
                    count++;
                }
                if (count > 0)
                    unix_sock_stage_fds(us->peer_id, staged, count);
            }
        }
    }

    return (uint64_t)total_sent;
}

uint64_t sys_recvmsg(uint64_t fd, uint64_t msg_ptr, uint64_t flags)
{
    (void)flags;
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t uid = unix_sock_id_from_fd((int)fd, proc);
    if (uid == UNIX_NONE) return (uint64_t)-(int64_t)38;  /* ENOSYS for non-unix */

    if (!user_ptr_valid(msg_ptr, sizeof(k_msghdr_t))) return (uint64_t)-(int64_t)14;
    k_msghdr_t mh;
    copy_from_user(&mh, (const void *)(uintptr_t)msg_ptr, sizeof(mh));

    /* Recv iov data */
    int64_t total_recv = 0;
    for (uint64_t i = 0; i < mh.msg_iovlen && i < 8; i++) {
        k_iovec_t iov;
        if (!user_ptr_valid((uint64_t)(uintptr_t)mh.msg_iov + i * sizeof(k_iovec_t), sizeof(k_iovec_t)))
            return (uint64_t)-(int64_t)14;
        copy_from_user(&iov, (const void *)((uintptr_t)mh.msg_iov + i * sizeof(k_iovec_t)), sizeof(iov));
        if (iov.iov_len == 0) continue;
        if (!user_ptr_valid((uint64_t)(uintptr_t)iov.iov_base, iov.iov_len))
            return (uint64_t)-(int64_t)14;
        uint8_t kbuf[1024];
        uint64_t remain = iov.iov_len;
        uint64_t off = 0;
        while (remain > 0) {
            uint32_t chunk = remain > 1024 ? 1024 : (uint32_t)remain;
            int n = unix_sock_read(uid, kbuf, chunk);
            if (n <= 0) {
                if (total_recv > 0) goto done;
                return n == 0 ? 0 : (uint64_t)(int64_t)n;
            }
            copy_to_user((void *)((uintptr_t)iov.iov_base + off), kbuf, (uint32_t)n);
            total_recv += n;
            off += (uint64_t)n;
            remain -= (uint64_t)n;
            if ((uint32_t)n < chunk) goto done;  /* partial read: don't block for more */
        }
    }
done:

    /* Receive staged fds if msg_control provided */
    if (mh.msg_control && mh.msg_controllen >= sizeof(k_cmsghdr_t) + sizeof(int)) {
        int received_fds[UNIX_PASSED_FD_MAX];
        int nfds = unix_sock_recv_fds(uid, received_fds, UNIX_PASSED_FD_MAX);
        if (nfds > 0) {
            /* Build cmsghdr + fd array */
            k_cmsghdr_t cm;
            cm.cmsg_len   = sizeof(k_cmsghdr_t) + (uint64_t)(nfds * sizeof(int));
            cm.cmsg_level = SOL_SOCKET;
            cm.cmsg_type  = SCM_RIGHTS;

            uint64_t total_cm = sizeof(k_cmsghdr_t) + (uint64_t)(nfds * sizeof(int));
            if (total_cm <= mh.msg_controllen) {
                copy_to_user((void *)(uintptr_t)mh.msg_control, &cm, sizeof(cm));
                copy_to_user((void *)((uintptr_t)mh.msg_control + sizeof(k_cmsghdr_t)),
                    received_fds, (uint32_t)(nfds * sizeof(int)));
                /* Update msg_controllen in user struct */
                copy_to_user((void *)((uintptr_t)msg_ptr + __builtin_offsetof(k_msghdr_t, msg_controllen)),
                    &total_cm, sizeof(uint64_t));
            }
        } else {
            /* No fds — zero out controllen */
            uint64_t zero = 0;
            copy_to_user((void *)((uintptr_t)msg_ptr + __builtin_offsetof(k_msghdr_t, msg_controllen)),
                &zero, sizeof(uint64_t));
        }
    }

    return (uint64_t)total_recv;
}

/* ── sys_shutdown ──────────────────────────────────────────────────────── */

uint64_t
sys_shutdown(uint64_t fd, uint64_t how)
{
    (void)how;
    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;

    if (s->type == SOCK_TYPE_STREAM && s->tcp_conn_id != SOCK_NONE) {
        tcp_conn_close(s->tcp_conn_id);
    }
    s->state = SOCK_CLOSED;
    return 0;
}

/* ── sys_getsockname / sys_getpeername ──────────────────────────────────── */

uint64_t
sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    if (!user_ptr_valid(addr, sizeof(k_sockaddr_in_t))) return (uint64_t)-(int64_t)14;
    if (!addrlen) return (uint64_t)-(int64_t)14;  /* EFAULT */
    if (!user_ptr_valid(addrlen, sizeof(uint32_t))) return (uint64_t)-(int64_t)14;  /* EFAULT */
    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;
    k_sockaddr_in_t sa;
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->local_port);
    sa.sin_addr   = s->local_ip;
    __builtin_memset(sa.sin_zero, 0, 8);
    uint32_t outlen = sizeof(sa);
    copy_to_user((void *)(uintptr_t)addrlen, &outlen, sizeof(uint32_t));
    copy_to_user((void *)(uintptr_t)addr, &sa, sizeof(sa));
    return 0;
}

uint64_t
sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    if (!user_ptr_valid(addr, sizeof(k_sockaddr_in_t))) return (uint64_t)-(int64_t)14;
    if (!addrlen) return (uint64_t)-(int64_t)14;  /* EFAULT */
    if (!user_ptr_valid(addrlen, sizeof(uint32_t))) return (uint64_t)-(int64_t)14;  /* EFAULT */
    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;
    if (s->state != SOCK_CONNECTED) return (uint64_t)-(int64_t)107;  /* ENOTCONN */
    k_sockaddr_in_t sa;
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->remote_port);
    sa.sin_addr   = s->remote_ip;
    __builtin_memset(sa.sin_zero, 0, 8);
    uint32_t outlen = sizeof(sa);
    copy_to_user((void *)(uintptr_t)addrlen, &outlen, sizeof(uint32_t));
    copy_to_user((void *)(uintptr_t)addr, &sa, sizeof(sa));
    return 0;
}

/* ── sys_socketpair ────────────────────────────────────────────────────── */

uint64_t
sys_socketpair(uint64_t domain, uint64_t type, uint64_t proto, uint64_t sv_ptr)
{
    (void)domain; (void)proto;
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return (uint64_t)-(int64_t)93;
    if (!user_ptr_valid(sv_ptr, 2 * sizeof(int)))
        return (uint64_t)-(int64_t)14;

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    int sid0 = sock_alloc(SOCK_TYPE_DGRAM);
    int sid1 = sock_alloc(SOCK_TYPE_DGRAM);
    if (sid0 < 0 || sid1 < 0) {
        if (sid0 >= 0) sock_free((uint32_t)sid0);
        if (sid1 >= 0) sock_free((uint32_t)sid1);
        return (uint64_t)-(int64_t)12;
    }
    sock_t *s0 = sock_get((uint32_t)sid0);
    sock_t *s1 = sock_get((uint32_t)sid1);
    s0->state = SOCK_CONNECTED; s0->remote_ip = 0x7f000001U; /* 127.0.0.1 */
    s1->state = SOCK_CONNECTED; s1->remote_ip = 0x7f000001U;
    /* Cross-link: peer_id stored in tcp_conn_id field (reused for socketpair) */
    s0->tcp_conn_id = (uint32_t)sid1;
    s1->tcp_conn_id = (uint32_t)sid0;

    int fd0 = sock_open_fd((uint32_t)sid0, proc);
    int fd1 = sock_open_fd((uint32_t)sid1, proc);
    if (fd0 < 0 || fd1 < 0) {
        sock_free((uint32_t)sid0); sock_free((uint32_t)sid1);
        return (uint64_t)-(int64_t)12;
    }
    int fds[2] = { fd0, fd1 };
    copy_to_user((void *)(uintptr_t)sv_ptr, fds, sizeof(fds));
    return 0;
}

/* ── sys_setsockopt ────────────────────────────────────────────────────── */

uint64_t
sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname,
               uint64_t optval, uint64_t optlen)
{
    if (optlen < sizeof(int)) return (uint64_t)-(int64_t)22;
    if (!user_ptr_valid(optval, sizeof(int))) return (uint64_t)-(int64_t)14;

    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;

    int val;
    copy_from_user(&val, (const void *)(uintptr_t)optval, sizeof(int));

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: s->reuseaddr = val ? 1 : 0; return 0;
        case SO_BROADCAST: s->broadcast = val ? 1 : 0; return 0;
        case SO_RCVTIMEO: {
            /* optval is struct timeval {tv_sec, tv_usec} — 16 bytes */
            if (optlen >= 16) {
                if (!user_ptr_valid(optval, 16))
                    return (uint64_t)-(int64_t)14; /* EFAULT */
                int64_t tv_sec;
                int64_t tv_usec;
                copy_from_user(&tv_sec,  (const void *)(uintptr_t)optval,     8);
                copy_from_user(&tv_usec, (const void *)(uintptr_t)(optval+8), 8);
                /* Convert to PIT ticks (100 Hz) */
                s->rcvtimeo_ticks = (uint32_t)(tv_sec * 100 + tv_usec / 10000);
            }
            return 0;
        }
        case SO_SNDTIMEO: {
            if (optlen >= 16) {
                if (!user_ptr_valid(optval, 16))
                    return (uint64_t)-(int64_t)14; /* EFAULT */
                int64_t tv_sec;
                int64_t tv_usec;
                copy_from_user(&tv_sec,  (const void *)(uintptr_t)optval,     8);
                copy_from_user(&tv_usec, (const void *)(uintptr_t)(optval+8), 8);
                s->sndtimeo_ticks = (uint32_t)(tv_sec * 100 + tv_usec / 10000);
            }
            return 0;
        }
        default: return (uint64_t)-(int64_t)92;  /* ENOPROTOOPT */
        }
    }
    if (level == IPPROTO_TCP && optname == TCP_NODELAY) {
        return 0;  /* TCP_NODELAY acknowledged but not implemented */
    }
    return (uint64_t)-(int64_t)92;  /* ENOPROTOOPT: unknown level */
}

/* ── sys_getsockopt ────────────────────────────────────────────────────── */

/* struct ucred for SO_PEERCRED */
typedef struct { int pid; int uid; int gid; } k_ucred_t;

uint64_t
sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname,
               uint64_t optval, uint64_t optlen)
{
    if (!user_ptr_valid(optval, sizeof(int))) return (uint64_t)-(int64_t)14;

    /* AF_UNIX: SO_PEERCRED */
    aegis_process_t *proc_g = (aegis_process_t *)sched_current();
    uint32_t uid = unix_sock_id_from_fd((int)fd, proc_g);
    if (uid != UNIX_NONE && level == SOL_SOCKET && optname == SO_PEERCRED) {
        if (!user_ptr_valid(optval, sizeof(k_ucred_t))) return (uint64_t)-(int64_t)14;
        uint32_t p_pid, p_uid, p_gid;
        int rc = unix_sock_peercred(uid, &p_pid, &p_uid, &p_gid);
        if (rc < 0) return (uint64_t)(int64_t)rc;
        k_ucred_t uc = { .pid = (int)p_pid, .uid = (int)p_uid, .gid = (int)p_gid };
        copy_to_user((void *)(uintptr_t)optval, &uc, sizeof(uc));
        uint32_t outlen = sizeof(uc);
        if (optlen) copy_to_user((void *)(uintptr_t)optlen, &outlen, sizeof(uint32_t));
        return 0;
    }

    sock_t *s; uint32_t sid;
    uint64_t err = get_proc_sock(fd, &s, &sid);
    if (err) return err;

    int val = 0;
    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: val = s->reuseaddr; break;
        case SO_BROADCAST: val = s->broadcast; break;
        default: break;
        }
    }
    copy_to_user((void *)(uintptr_t)optval, &val, sizeof(int));
    uint32_t outlen = sizeof(int);
    if (optlen) copy_to_user((void *)(uintptr_t)optlen, &outlen, sizeof(uint32_t));
    return 0;
}

/* ── sys_poll ──────────────────────────────────────────────────────────── */

/* struct pollfd layout (Linux ABI) */
typedef struct {
    int      fd;
    uint16_t events;
    uint16_t revents;
} k_pollfd_t;

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

uint64_t
sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms)
{
    if (nfds > 64) return (uint64_t)-(int64_t)22;
    if (!user_ptr_valid(fds_ptr, nfds * sizeof(k_pollfd_t))) return (uint64_t)-(int64_t)14;

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t timeout_ticks = (timeout_ms == (uint64_t)-1) ? 0xFFFFFFFFU :
                             (timeout_ms == 0) ? 0 :
                             (uint32_t)(timeout_ms / 10);  /* ms → ticks at 100 Hz */
    uint32_t deadline = (timeout_ticks > 0 && timeout_ticks != 0xFFFFFFFFU) ?
                        (uint32_t)arch_get_ticks() + timeout_ticks : 0;

    for (;;) {
        int ready = 0;
        uint64_t i;
        for (i = 0; i < nfds; i++) {
            k_pollfd_t pfd;
            copy_from_user(&pfd, (const void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                           sizeof(k_pollfd_t));
            pfd.revents = 0;
            uint32_t sid = sock_id_from_fd(pfd.fd, proc);
            if (sid != SOCK_NONE) {
                /* Socket fd — use existing socket poll logic */
                sock_t *s = sock_get(sid);
                if (s) {
                    if (s->type == SOCK_TYPE_STREAM && s->state == SOCK_CONNECTED) {
                        tcp_conn_t *tc = tcp_conn_get(s->tcp_conn_id);
                        int peek = tcp_conn_recv(s->tcp_conn_id, (void *)0, 0);
                        /* POLLIN: data available OR EOF (recv will return 0) */
                        if ((pfd.events & POLLIN) && (peek > 0 || peek == 0))
                            pfd.revents |= POLLIN;
                        if (pfd.events & POLLOUT)
                            pfd.revents |= POLLOUT;
                        /* POLLHUP: TCP connection closed (FIN received or RST) */
                        if (tc && (tc->state == TCP_CLOSE_WAIT || tc->state == TCP_CLOSED
                                   || tc->state == TCP_TIME_WAIT))
                            pfd.revents |= POLLHUP;
                    } else if (s->type == SOCK_TYPE_STREAM && s->state == SOCK_LISTENING) {
                        if ((pfd.events & POLLIN) && s->accept_head != s->accept_tail)
                            pfd.revents |= POLLIN;
                    } else if (s->type == SOCK_TYPE_DGRAM) {
                        if ((pfd.events & POLLIN) && s->udp_rx_head != s->udp_rx_tail)
                            pfd.revents |= POLLIN;
                        if (pfd.events & POLLOUT)
                            pfd.revents |= POLLOUT;
                    }
                }
            } else if (pfd.fd >= 0 && (uint32_t)pfd.fd < PROC_MAX_FDS &&
                       proc->fd_table->fds[pfd.fd].ops) {
                /* VFS fd — use .poll callback */
                const vfs_ops_t *ops = proc->fd_table->fds[pfd.fd].ops;
                if (ops->poll) {
                    uint16_t r = ops->poll(proc->fd_table->fds[pfd.fd].priv);
                    pfd.revents = r & (pfd.events | POLLERR | POLLHUP);
                } else {
                    /* No .poll — permissive default */
                    pfd.revents = pfd.events & (POLLIN | POLLOUT);
                }
            } else {
                pfd.revents = POLLNVAL;
            }
            if (pfd.revents) ready++;
            /* revents already counted in ready above */
            copy_to_user((void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                         &pfd, sizeof(k_pollfd_t));
        }
        if (ready > 0 || timeout_ticks == 0) return (uint64_t)ready;
        if (deadline && (uint32_t)arch_get_ticks() >= deadline) return 0;
        /* Block on g_timer_waitq — PIT wakes it each tick so we re-check
         * deadline + fd readiness on the next iteration. */
        waitq_wait(&g_timer_waitq);
    }
}

/* ── sys_select — minimal stub ──────────────────────────────────────────── */

uint64_t
sys_select(uint64_t nfds, uint64_t rfds, uint64_t wfds,
           uint64_t efds, uint64_t timeout)
{
    (void)nfds; (void)rfds; (void)wfds; (void)efds; (void)timeout;
    return (uint64_t)-38;  /* ENOSYS — select not implemented */
}

/* ── sys_epoll_create1 ──────────────────────────────────────────────────── */

uint64_t
sys_epoll_create1(uint64_t flags)
{
    (void)flags;
    int eid = epoll_alloc();
    if (eid < 0) return (uint64_t)-(int64_t)24;  /* EMFILE */
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    int fd = epoll_open_fd((uint32_t)eid, proc);
    if (fd < 0) { epoll_free((uint32_t)eid); return (uint64_t)-(int64_t)24; }
    return (uint64_t)fd;
}

/* ── sys_epoll_ctl ──────────────────────────────────────────────────────── */

uint64_t
sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event_ptr)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t eid = epoll_id_from_fd((int)epfd, proc);
    if (eid == EPOLL_NONE) return (uint64_t)-(int64_t)9;  /* EBADF */

    k_epoll_event_t ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    if (event_ptr && !user_ptr_valid(event_ptr, sizeof(k_epoll_event_t)))
        return (uint64_t)-(int64_t)14;
    if (event_ptr)
        copy_from_user(&ev, (const void *)(uintptr_t)event_ptr, sizeof(k_epoll_event_t));

    int r = epoll_ctl_impl(eid, (int)op, (int)fd, &ev);
    return r < 0 ? (uint64_t)(int64_t)r : 0;
}

/* ── sys_epoll_wait ──────────────────────────────────────────────────────── */

uint64_t
sys_epoll_wait(uint64_t epfd, uint64_t events_ptr, uint64_t maxevents, uint64_t timeout_ms)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t eid = epoll_id_from_fd((int)epfd, proc);
    if (eid == EPOLL_NONE) return (uint64_t)-(int64_t)9;  /* EBADF */
    if (maxevents > EPOLL_MAX_WATCHES) return (uint64_t)-(int64_t)22;  /* EINVAL */
    if (!user_ptr_valid(events_ptr, (uint64_t)maxevents * sizeof(k_epoll_event_t)))
        return (uint64_t)-(int64_t)14;

    uint32_t ticks = (timeout_ms == (uint64_t)-1) ? 0xFFFFFFFFU :
                     (timeout_ms == 0) ? 0 :
                     (uint32_t)(timeout_ms / 10);
    int r = epoll_wait_impl(eid, events_ptr, (int)maxevents, ticks);
    return r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
}

/* ── sys_netcfg ─────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
} netcfg_info_t;

uint64_t
sys_netcfg(uint64_t op, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (op == 0) {
        /* op=0 (set IP/mask/gw) is privileged — requires NET_ADMIN. */
        if (cap_check(proc->caps, CAP_TABLE_SIZE,
                      CAP_KIND_NET_ADMIN, CAP_RIGHTS_WRITE) != 0)
            return (uint64_t)-(int64_t)130;  /* ENOCAP */
        /* Set IP/mask/gw */
        net_set_config((ip4_addr_t)arg1, (ip4_addr_t)arg2, (ip4_addr_t)arg3);

        /* Proactively resolve gateway ARP so it's cached before any TCP
         * connections arrive.  Without this, the first inbound TCP SYN
         * triggers arp_resolve from the PIT ISR (via tcp_rx → ip_send),
         * which would deadlock if ARP isn't cached.  Safe to call here:
         * syscall context, interrupts enabled, no spinlocks held. */
        if (arg3 != 0) {
            netdev_t *dev = netdev_get("eth0");
            if (dev) {
                mac_addr_t gw_mac;
                arp_resolve(dev, (ip4_addr_t)arg3, &gw_mac);
            }
        }
        return 0;
    }
    if (op == 1) {
        /* Read current config + MAC */
        if (!user_ptr_valid(arg1, sizeof(netcfg_info_t))) return (uint64_t)-(int64_t)14;
        netdev_t *dev = netdev_get("eth0");
        if (!dev) return (uint64_t)-(int64_t)19;  /* ENODEV */
        netcfg_info_t info;
        __builtin_memset(&info, 0, sizeof(info));
        uint32_t i;
        for (i = 0; i < 6; i++) info.mac[i] = dev->mac[i];
        ip4_addr_t ip, mask, gw;
        net_get_config(&ip, &mask, &gw);
        info.ip      = ip;
        info.mask    = mask;
        info.gateway = gw;
        copy_to_user((void *)(uintptr_t)arg1, &info, sizeof(info));
        return 0;
    }
    return (uint64_t)-(int64_t)22;  /* EINVAL */
}
