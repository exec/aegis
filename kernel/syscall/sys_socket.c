/* sys_socket.c — POSIX socket API syscalls */
#include "sys_impl.h"
#include "netdev.h"
#include "udp.h"
#include "tcp.h"
#include "ip.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define SOCK_STREAM    1
#define SOCK_DGRAM     2

#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_BROADCAST   6
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21

#define IPPROTO_TCP    6
#define TCP_NODELAY    1

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
    sock_t *ls; uint32_t lsid;
    uint64_t err = get_proc_sock(fd, &ls, &lsid);
    if (err) return err;
    if (ls->state != SOCK_LISTENING) return (uint64_t)-(int64_t)22;

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    for (;;) {
        /* Check accept queue */
        if (ls->accept_head != ls->accept_tail) {
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

        if (ls->nonblocking) return (uint64_t)-(int64_t)11;  /* EAGAIN */

        ls->waiter_task = (aegis_task_t *)sched_current();
        sched_block();
    }
}

/* ── sys_connect ───────────────────────────────────────────────────────── */

uint64_t
sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
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

    /* TCP: initiate SYN */
    uint32_t conn_id;
    int r = tcp_connect(sid, sa.sin_addr, ntohs(sa.sin_port), &conn_id);
    if (r < 0) return (uint64_t)(int64_t)r;
    s->tcp_conn_id = conn_id;
    s->state       = SOCK_CONNECTING;

    /* Block until ESTABLISHED */
    s->waiter_task = (aegis_task_t *)sched_current();
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
        static uint8_t s_sndbuf[1460];
        uint64_t sent = 0;
        while (sent < len) {
            uint64_t chunk = len - sent;
            if (chunk > 1460) chunk = 1460;
            copy_from_user(s_sndbuf, (const void *)(uintptr_t)(buf + sent), (uint32_t)chunk);
            int n = tcp_conn_send(s->tcp_conn_id, s_sndbuf, (uint16_t)chunk);
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
        /* TCP recv */
        for (;;) {
            int n = tcp_conn_recv(s->tcp_conn_id, (void *)0, 0);  /* peek: available bytes */
            if (n > 0) {
                static uint8_t s_rcvbuf[8192];
                uint32_t want = (uint32_t)len < (uint32_t)n ? (uint32_t)len : (uint32_t)n;
                if (want > 8192) want = 8192;
                n = tcp_conn_recv(s->tcp_conn_id, s_rcvbuf, (uint16_t)want);
                if (n > 0) {
                    copy_to_user((void *)(uintptr_t)buf, s_rcvbuf, (uint32_t)n);
                    return (uint64_t)n;
                }
            }
            if (n == 0) return 0;  /* EOF / FIN */
            if (s->nonblocking) return (uint64_t)-(int64_t)11;  /* EAGAIN */
            if (has_timeout && (uint32_t)arch_get_ticks() >= deadline)
                return (uint64_t)-(int64_t)110;  /* ETIMEDOUT */
            s->waiter_task = (aegis_task_t *)sched_current();
            sched_block();
        }
    }

    /* UDP recv */
    for (;;) {
        if (s->udp_rx_head != s->udp_rx_tail) {
            udp_rx_slot_t *slot = &s->udp_rx[s->udp_rx_head];
            s->udp_rx_head = (s->udp_rx_head + 1) & (UDP_RX_SLOTS - 1);
            uint32_t copy_len = slot->len < (uint32_t)len ? slot->len : (uint32_t)len;
            copy_to_user((void *)(uintptr_t)buf, slot->data, copy_len);
            if (addr && addrlen >= sizeof(k_sockaddr_in_t)) {
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
        if (s->nonblocking) return (uint64_t)-(int64_t)11;  /* EAGAIN */
        if (has_timeout && (uint32_t)arch_get_ticks() >= deadline)
            return (uint64_t)-(int64_t)110;  /* ETIMEDOUT */
        s->waiter_task = (aegis_task_t *)sched_current();
        sched_block();
    }
}

/* ── sys_sendmsg / sys_recvmsg — minimal stubs ─────────────────────────── */

uint64_t sys_sendmsg(uint64_t fd, uint64_t msg, uint64_t flags)
{
    (void)fd; (void)msg; (void)flags;
    return (uint64_t)-(int64_t)38;
}

uint64_t sys_recvmsg(uint64_t fd, uint64_t msg, uint64_t flags)
{
    (void)fd; (void)msg; (void)flags;
    return (uint64_t)-(int64_t)38;
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
                int64_t tv_sec;
                int64_t tv_usec;
                copy_from_user(&tv_sec,  (const void *)(uintptr_t)optval,     8);
                copy_from_user(&tv_usec, (const void *)(uintptr_t)(optval+8), 8);
                s->sndtimeo_ticks = (uint32_t)(tv_sec * 100 + tv_usec / 10000);
            }
            return 0;
        }
        default: return 0;  /* ignore unknown options gracefully */
        }
    }
    if (level == IPPROTO_TCP && optname == TCP_NODELAY) {
        return 0;  /* TCP_NODELAY acknowledged but not implemented */
    }
    return 0;  /* unknown level: succeed silently */
}

/* ── sys_getsockopt ────────────────────────────────────────────────────── */

uint64_t
sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname,
               uint64_t optval, uint64_t optlen)
{
    if (!user_ptr_valid(optval, sizeof(int))) return (uint64_t)-(int64_t)14;
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
                sock_t *s = sock_get(sid);
                if (s) {
                    if ((pfd.events & POLLIN) &&
                        (s->udp_rx_head != s->udp_rx_tail ||
                         (s->type == SOCK_TYPE_STREAM && s->state == SOCK_LISTENING &&
                          s->accept_head != s->accept_tail) ||
                         (s->type == SOCK_TYPE_STREAM && s->state == SOCK_CONNECTED &&
                          tcp_conn_recv(s->tcp_conn_id, (void *)0, 0) > 0)))
                        pfd.revents |= POLLIN;
                    if (pfd.events & POLLOUT) pfd.revents |= POLLOUT;
                }
            }
            if (pfd.revents) ready++;
            copy_to_user((void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                         &pfd, sizeof(k_pollfd_t));
        }
        if (ready > 0 || timeout_ticks == 0) return (uint64_t)ready;
        if (deadline && (uint32_t)arch_get_ticks() >= deadline) return 0;
        /* Sleep one tick and retry */
        sched_block();
    }
}

/* ── sys_select — minimal stub ──────────────────────────────────────────── */

uint64_t
sys_select(uint64_t nfds, uint64_t rfds, uint64_t wfds,
           uint64_t efds, uint64_t timeout)
{
    (void)nfds; (void)rfds; (void)wfds; (void)efds; (void)timeout;
    return 0;
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
    if (cap_check(proc->caps, CAP_TABLE_SIZE, CAP_KIND_NET_ADMIN, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)130;  /* ENOCAP */

    if (op == 0) {
        /* Set IP/mask/gw */
        net_set_config((ip4_addr_t)arg1, (ip4_addr_t)arg2, (ip4_addr_t)arg3);
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
