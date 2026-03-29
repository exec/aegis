/* kernel/net/tcp.c — TCP state machine (RFC 793), retransmit, TIME_WAIT */
#include "tcp.h"
#include "ip.h"
#include "socket.h"
#include "epoll.h"
#include "arch.h"   /* arch_get_ticks() */
#include "../core/printk.h"
#include "../core/spinlock.h"
#include <stddef.h>                 /* NULL */

/* S2: RFC 793 serial number arithmetic for TCP sequence numbers.
 * 32-bit sequence numbers wrap; plain < / > gives wrong results near 2^31.
 * a < b iff the signed difference (a - b) is negative. */
static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

/* Local memory helpers. */
static void _tcp_memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void _tcp_memcpy(void *dst, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    while (n--) *d++ = *s++;
}

/* TCP header (20 bytes, no options). */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

typedef struct __attribute__((packed)) {
    ip4_addr_t src;
    ip4_addr_t dst;
    uint8_t    zero;
    uint8_t    proto;
    uint16_t   tcp_len;
} tcp_pseudo_hdr_t;

static tcp_conn_t s_tcp[TCP_MAX_CONNS];
static uint8_t s_tcp_buf[1480];
/* RST template — used by tcp_rx to send RST|ACK for unknown ports.
 * File-static to avoid placing a 16KB tcp_conn_t on the ISR stack. */
static tcp_conn_t s_rst_conn;
static spinlock_t tcp_lock = SPINLOCK_INIT;

void tcp_init(void)
{
    _tcp_memset(s_tcp, 0, sizeof(s_tcp));
}

int tcp_send_segment(netdev_t *dev, tcp_conn_t *conn,
                     uint8_t flags, const void *payload, uint16_t len)
{
    if (!dev) return -1;
    uint16_t tcp_len = (uint16_t)(sizeof(tcp_hdr_t) + len);
    if (tcp_len > (uint16_t)sizeof(s_tcp_buf)) return -1;

    tcp_hdr_t *hdr = (tcp_hdr_t *)s_tcp_buf;
    hdr->src_port = htons(conn->local_port);
    hdr->dst_port = htons(conn->remote_port);
    hdr->seq      = htonl(conn->snd_nxt);
    hdr->ack      = (flags & TCP_ACK) ? htonl(conn->rcv_nxt) : 0;
    hdr->data_off = (uint8_t)((sizeof(tcp_hdr_t) / 4) << 4);
    hdr->flags    = flags;
    /* Advertise available receive buffer space so the sender paces correctly. */
    {
        uint32_t used  = (conn->rbuf_tail - conn->rbuf_head) & (TCP_RBUF_SIZE - 1);
        uint32_t avail = TCP_RBUF_SIZE - used;
        if (avail > 0xFFFFu) avail = 0xFFFFu;
        hdr->window = htons((uint16_t)avail);
    }
    hdr->checksum = 0;
    hdr->urgent   = 0;
    if (payload && len > 0)
        _tcp_memcpy(s_tcp_buf + sizeof(tcp_hdr_t), payload, len);

    tcp_pseudo_hdr_t ph;
    ph.src      = conn->local_ip;
    ph.dst      = conn->remote_ip;
    ph.zero     = 0;
    ph.proto    = IP_PROTO_TCP;
    ph.tcp_len  = htons(tcp_len);
    uint32_t sum = 0;
    sum += net_checksum(&ph, sizeof(ph));
    sum += net_checksum(s_tcp_buf, tcp_len);
    hdr->checksum = net_checksum_finish(sum);

    return ip_send(dev, conn->remote_ip, IP_PROTO_TCP, s_tcp_buf, tcp_len);
}

static tcp_conn_t *tcp_find(ip4_addr_t remote_ip, uint16_t remote_port,
                             ip4_addr_t local_ip,  uint16_t local_port)
{
    int i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &s_tcp[i];
        if (c->state       != TCP_CLOSED  &&
            c->remote_ip   == remote_ip   &&
            c->remote_port == remote_port &&
            c->local_ip    == local_ip    &&
            c->local_port  == local_port)
            return c;
    }
    return NULL;
}

static tcp_conn_t *tcp_find_listener(ip4_addr_t local_ip, uint16_t local_port)
{
    int i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &s_tcp[i];
        if (c->state == TCP_LISTEN &&
            c->local_port == local_port &&
            (c->local_ip == 0 || c->local_ip == local_ip))
            return c;
    }
    return NULL;
}

static tcp_conn_t *tcp_alloc(void)
{
    int i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (s_tcp[i].state == TCP_CLOSED) {
            _tcp_memset(&s_tcp[i], 0, sizeof(s_tcp[i]));
            return &s_tcp[i];
        }
    }
    return NULL;
}

void tcp_rx(netdev_t *dev, ip4_addr_t src_ip, ip4_addr_t dst_ip,
            const void *tcp_data, uint16_t len)
{
    if (len < (uint16_t)sizeof(tcp_hdr_t)) return;
    irqflags_t fl = spin_lock_irqsave(&tcp_lock);
    const tcp_hdr_t *seg = (const tcp_hdr_t *)tcp_data;

    /* Deferred socket wake/notify — collected under tcp_lock, executed after
     * release.  Avoids tcp_lock → sock_lock ordering inversion. */
    #define TCP_RX_WAKE_MAX 4
    uint32_t wake_ids[TCP_RX_WAKE_MAX];
    uint32_t wake_epoll_events[TCP_RX_WAKE_MAX];
    uint32_t wake_count = 0;
    /* Deferred sock state transition (SYN_SENT → CONNECTED). */
    uint32_t connect_sock_id = SOCK_NONE;
    /* Deferred accept queue push (SYN_RCVD → ESTABLISHED). */
    uint32_t accept_listener_id = SOCK_NONE;
    uint32_t accept_conn_id = 0;

    tcp_pseudo_hdr_t ph;
    ph.src     = src_ip;
    ph.dst     = dst_ip;
    ph.zero    = 0;
    ph.proto   = IP_PROTO_TCP;
    ph.tcp_len = htons(len);
    uint32_t sum = 0;
    sum += net_checksum(&ph, sizeof(ph));
    sum += net_checksum(tcp_data, len);
    if (net_checksum_finish(sum) != 0) goto out;

    uint16_t remote_port = ntohs(seg->src_port);
    uint16_t local_port  = ntohs(seg->dst_port);
    uint8_t  hdr_bytes   = (uint8_t)((seg->data_off >> 4) * 4);
    if (hdr_bytes < sizeof(tcp_hdr_t) || hdr_bytes > len) goto out;
    uint16_t payload_len = (uint16_t)(len - hdr_bytes);
    const uint8_t *payload = (const uint8_t *)tcp_data + hdr_bytes;
    uint32_t seq = ntohl(seg->seq);
    uint32_t ack = ntohl(seg->ack);
    uint8_t  flags = seg->flags;

    tcp_conn_t *conn = tcp_find(src_ip, remote_port, dst_ip, local_port);

    if (!conn) {
        if (flags & TCP_SYN) {
            tcp_conn_t *listener = tcp_find_listener(dst_ip, local_port);
            if (!listener) {
                _tcp_memset(&s_rst_conn, 0, sizeof(s_rst_conn));
                s_rst_conn.local_ip    = dst_ip;
                s_rst_conn.local_port  = local_port;
                s_rst_conn.remote_ip   = src_ip;
                s_rst_conn.remote_port = remote_port;
                s_rst_conn.snd_nxt     = 0;
                s_rst_conn.rcv_nxt     = seq + 1;
                tcp_send_segment(dev, &s_rst_conn, TCP_RST | TCP_ACK, NULL, 0);
                goto out;
            }
            conn = tcp_alloc();
            if (!conn) goto out;
            conn->state       = TCP_SYN_RCVD;
            conn->dev         = dev;
            conn->local_ip    = dst_ip;
            conn->local_port  = local_port;
            conn->remote_ip   = src_ip;
            conn->remote_port = remote_port;
            conn->rcv_nxt     = seq + 1;
            conn->snd_nxt     = (uint32_t)arch_get_ticks();
            conn->snd_una     = conn->snd_nxt;
            conn->snd_wnd     = ntohs(seg->window);
            conn->listener_id = listener->sock_id;
            tcp_send_segment(dev, conn, TCP_SYN | TCP_ACK, NULL, 0);
            conn->snd_nxt++;
            conn->retransmit_at    = (uint32_t)arch_get_ticks() + TCP_RTO_INITIAL;
            conn->retransmit_count = 0;
        }
        goto out;
    }

    conn->snd_wnd = ntohs(seg->window);

    switch (conn->state) {
    case TCP_SYN_RCVD:
        if ((flags & TCP_ACK) && !(flags & TCP_SYN) && !(flags & TCP_RST)) {
            if (ack == conn->snd_nxt) {
                conn->snd_una  = ack;
                conn->state    = TCP_ESTABLISHED;
                printk("[TCP] ESTABLISHED port=%u\n", (uint32_t)conn->local_port);
                conn->retransmit_at = 0;
                /* Defer accept() queue push + wake to after tcp_lock release */
                if (conn->listener_id != SOCK_NONE) {
                    accept_listener_id = conn->listener_id;
                    accept_conn_id = (uint32_t)(conn - s_tcp);
                    if (wake_count < TCP_RX_WAKE_MAX) {
                        wake_ids[wake_count] = conn->listener_id;
                        wake_epoll_events[wake_count] = EPOLLIN;
                        wake_count++;
                    }
                }
            }
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == conn->snd_nxt) {
                conn->snd_una  = ack;
                conn->rcv_nxt  = seq + 1;
                conn->state    = TCP_ESTABLISHED;
                tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
                conn->retransmit_at = 0;
                /* Defer connect() wake + state transition */
                if (conn->sock_id != SOCK_NONE) {
                    connect_sock_id = conn->sock_id;
                    if (wake_count < TCP_RX_WAKE_MAX) {
                        wake_ids[wake_count] = conn->sock_id;
                        wake_epoll_events[wake_count] = EPOLLOUT;
                        wake_count++;
                    }
                }
            }
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_ACK) {
            if (seq_gt(ack, conn->snd_una))
                conn->snd_una = ack;
        }
        if (payload_len > 0 && seq == conn->rcv_nxt) {
            uint32_t space = TCP_RBUF_SIZE -
                ((conn->rbuf_tail - conn->rbuf_head) & (TCP_RBUF_SIZE - 1));
            if (payload_len <= space) {
                uint16_t i;
                for (i = 0; i < payload_len; i++) {
                    conn->rbuf[conn->rbuf_tail & (TCP_RBUF_SIZE - 1)] = payload[i];
                    conn->rbuf_tail++;
                }
                conn->rcv_nxt += payload_len;
                /* Defer recv() wake */
                if (conn->sock_id != SOCK_NONE &&
                    wake_count < TCP_RX_WAKE_MAX) {
                    wake_ids[wake_count] = conn->sock_id;
                    wake_epoll_events[wake_count] = EPOLLIN;
                    wake_count++;
                }
            }
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_RST) {
            conn->state = TCP_CLOSED;
            if (conn->sock_id != SOCK_NONE &&
                wake_count < TCP_RX_WAKE_MAX) {
                wake_ids[wake_count] = conn->sock_id;
                wake_epoll_events[wake_count] = 0; /* wake only, no epoll */
                wake_count++;
            }
            break;
        }
        if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
            /* Defer recv() EOF/hangup wake */
            if (conn->sock_id != SOCK_NONE &&
                wake_count < TCP_RX_WAKE_MAX) {
                wake_ids[wake_count] = conn->sock_id;
                wake_epoll_events[wake_count] = EPOLLHUP;
                wake_count++;
            }
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK) {
            if (ack == conn->snd_nxt) {
                conn->snd_una = ack;
                if (flags & TCP_FIN) {
                    conn->rcv_nxt++;
                    conn->state = TCP_CLOSING;
                    tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
                } else {
                    conn->state = TCP_FIN_WAIT_2;
                }
            }
        } else if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            conn->state = TCP_CLOSING;
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            conn->state       = TCP_TIME_WAIT;
            conn->timewait_at = (uint32_t)arch_get_ticks() + TCP_TIMEWAIT_TICKS;
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
            if (conn->sock_id != SOCK_NONE &&
                wake_count < TCP_RX_WAKE_MAX) {
                wake_ids[wake_count] = conn->sock_id;
                wake_epoll_events[wake_count] = EPOLLHUP;
                wake_count++;
            }
        }
        break;

    case TCP_CLOSING:
        if (flags & TCP_ACK) {
            conn->state       = TCP_TIME_WAIT;
            conn->timewait_at = (uint32_t)arch_get_ticks() + TCP_TIMEWAIT_TICKS;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            conn->state = TCP_CLOSED;
        }
        break;

    default:
        break;
    }
out:
    spin_unlock_irqrestore(&tcp_lock, fl);

    /* ── Deferred socket operations (outside tcp_lock) ───────────────────
     * sock_get/sock_wake acquire sock_lock.  Calling them under tcp_lock
     * would violate the lock ordering (sock_lock > tcp_lock). */

    /* SYN_SENT → CONNECTED: update sock state before waking. */
    if (connect_sock_id != SOCK_NONE) {
        sock_t *sk = sock_get(connect_sock_id);
        if (sk) sk->state = SOCK_CONNECTED;
    }

    /* Accept queue push for SYN_RCVD → ESTABLISHED. */
    if (accept_listener_id != SOCK_NONE) {
        sock_t *ls = sock_get(accept_listener_id);
        if (ls) {
            uint8_t next_tail = (uint8_t)((ls->accept_tail + 1) & 7);
            if (next_tail != ls->accept_head) {
                ls->accept_queue[ls->accept_tail] = accept_conn_id;
                ls->accept_tail = next_tail;
            }
        }
    }

    {
        uint32_t w;
        for (w = 0; w < wake_count; w++) {
            sock_wake(wake_ids[w]);
            if (wake_epoll_events[w] != 0)
                epoll_notify(wake_ids[w], wake_epoll_events[w]);
        }
    }
    #undef TCP_RX_WAKE_MAX
}

void tcp_tick(void)
{
    irqflags_t fl = spin_lock_irqsave(&tcp_lock);
    uint32_t now = (uint32_t)arch_get_ticks();
    int i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &s_tcp[i];
        if (c->state == TCP_CLOSED) continue;

        if (c->state == TCP_TIME_WAIT) {
            if ((int32_t)(now - c->timewait_at) >= 0)
                c->state = TCP_CLOSED;
            continue;
        }

        if (c->retransmit_at == 0) continue;
        if ((int32_t)(now - c->retransmit_at) < 0) continue;

        if (c->retransmit_count >= TCP_RETRANSMIT_MAX) {
            /* Maximum retransmits — RST best-effort and close. */
            tcp_send_segment(c->dev, c, TCP_RST, NULL, 0);
            c->state = TCP_CLOSED;
            continue;
        }

        c->retransmit_count++;
        uint32_t rto = TCP_RTO_INITIAL << c->retransmit_count;
        if (rto > TCP_RTO_MAX) rto = TCP_RTO_MAX;
        c->retransmit_at = now + rto;

        if (c->state == TCP_SYN_RCVD)
            tcp_send_segment(c->dev, c, TCP_SYN | TCP_ACK, NULL, 0);
        else if (c->state == TCP_SYN_SENT)
            tcp_send_segment(c->dev, c, TCP_SYN, NULL, 0);
    }
    spin_unlock_irqrestore(&tcp_lock, fl);
}

/* ── Socket-layer helpers (Phase 26) ─────────────────────────────────────── */

/* tcp_listen: register a listening socket at port. */
int
tcp_listen(uint16_t port, uint32_t sock_id)
{
    irqflags_t fl = spin_lock_irqsave(&tcp_lock);
    uint32_t i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (s_tcp[i].state == TCP_CLOSED) {
            _tcp_memset(&s_tcp[i], 0, sizeof(s_tcp[i]));
            s_tcp[i].state      = TCP_LISTEN;
            s_tcp[i].local_port = port;
            s_tcp[i].sock_id    = sock_id;
            spin_unlock_irqrestore(&tcp_lock, fl);
            return 0;
        }
    }
    spin_unlock_irqrestore(&tcp_lock, fl);
    return -1;
}

/* tcp_connect: send SYN, allocate conn. Returns 0 on success. */
int
tcp_connect(uint32_t sock_id, ip4_addr_t dst_ip, uint16_t dst_port,
            uint32_t *conn_id_out)
{
    extern netdev_t *netdev_get(const char *name);
    irqflags_t fl = spin_lock_irqsave(&tcp_lock);
    uint32_t i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (s_tcp[i].state == TCP_CLOSED) {
            _tcp_memset(&s_tcp[i], 0, sizeof(s_tcp[i]));
            s_tcp[i].state       = TCP_SYN_SENT;
            net_get_config(&s_tcp[i].local_ip, (ip4_addr_t *)0, (ip4_addr_t *)0);
            s_tcp[i].local_port  = (uint16_t)(49152u + (arch_get_ticks() & 0x3FFFu));
            s_tcp[i].remote_ip   = dst_ip;
            s_tcp[i].remote_port = dst_port;
            s_tcp[i].sock_id     = sock_id;
            s_tcp[i].snd_nxt     = (uint32_t)arch_get_ticks();
            s_tcp[i].snd_una     = s_tcp[i].snd_nxt;
            *conn_id_out = i;
            netdev_t *dev = netdev_get("eth0");
            s_tcp[i].dev = dev;
            s_tcp[i].snd_nxt++;
            s_tcp[i].retransmit_at = (uint32_t)arch_get_ticks() + TCP_RTO_INITIAL;
            /* Release tcp_lock BEFORE sending SYN.  tcp_send_segment calls
             * ip_send → arp_resolve, which may block waiting for an ARP reply.
             * Holding tcp_lock across that call deadlocks: the PIT ISR calls
             * tcp_tick which tries to acquire tcp_lock. */
            spin_unlock_irqrestore(&tcp_lock, fl);
            tcp_send_segment(dev, &s_tcp[i], TCP_SYN, (void *)0, 0);
            return 0;
        }
    }
    spin_unlock_irqrestore(&tcp_lock, fl);
    return -1;
}

/* tcp_conn_recv: read up to max_len bytes from rbuf. max_len=0 returns available count. */
int
tcp_conn_recv(uint32_t conn_id, void *dst, uint16_t max_len)
{
    irqflags_t fl = spin_lock_irqsave(&tcp_lock);
    if (conn_id >= TCP_MAX_CONNS) {
        spin_unlock_irqrestore(&tcp_lock, fl);
        return -1;
    }
    tcp_conn_t *c = &s_tcp[conn_id];
    uint32_t avail = (c->rbuf_tail - c->rbuf_head + TCP_RBUF_SIZE) % TCP_RBUF_SIZE;
    if (avail == 0) {
        int ret;
        if (c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED
            || c->state == TCP_TIME_WAIT)
            ret = 0;  /* EOF — FIN received, no more data */
        else
            ret = -11;  /* EAGAIN — buffer empty, connection alive */
        spin_unlock_irqrestore(&tcp_lock, fl);
        return ret;
    }
    if (max_len == 0) {
        spin_unlock_irqrestore(&tcp_lock, fl);
        return (int)avail;  /* peek: report available bytes */
    }
    uint32_t n = avail < max_len ? avail : max_len;
    uint8_t *d = (uint8_t *)dst;
    uint32_t j;
    for (j = 0; j < n; j++) {
        d[j] = c->rbuf[c->rbuf_head];
        c->rbuf_head = (c->rbuf_head + 1) % TCP_RBUF_SIZE;
    }
    spin_unlock_irqrestore(&tcp_lock, fl);
    return (int)n;
}

/* tcp_conn_send: copy data into sbuf and send. */
int
tcp_conn_send(uint32_t conn_id, const void *data, uint16_t len)
{
    irqflags_t fl = spin_lock_irqsave(&tcp_lock);
    if (conn_id >= TCP_MAX_CONNS) {
        spin_unlock_irqrestore(&tcp_lock, fl);
        return -1;
    }
    tcp_conn_t *c = &s_tcp[conn_id];
    if (c->state != TCP_ESTABLISHED) {
        spin_unlock_irqrestore(&tcp_lock, fl);
        return -32;  /* EPIPE */
    }
    int r = tcp_send_segment(c->dev, c, TCP_PSH | TCP_ACK, data, len);
    if (r == 0) {
        c->snd_nxt += len;
        spin_unlock_irqrestore(&tcp_lock, fl);
        return (int)len;
    }
    spin_unlock_irqrestore(&tcp_lock, fl);
    return -1;
}

/* tcp_conn_close: send FIN. */
int
tcp_conn_close(uint32_t conn_id)
{
    irqflags_t fl = spin_lock_irqsave(&tcp_lock);
    if (conn_id >= TCP_MAX_CONNS) {
        spin_unlock_irqrestore(&tcp_lock, fl);
        return -1;
    }
    tcp_conn_t *c = &s_tcp[conn_id];
    if (c->state == TCP_ESTABLISHED) {
        tcp_send_segment(c->dev, c, TCP_FIN | TCP_ACK, (void *)0, 0);
        c->state = TCP_FIN_WAIT_1;
        c->snd_nxt++;
    }
    spin_unlock_irqrestore(&tcp_lock, fl);
    return 0;
}

/* tcp_conn_get_addr: fill remote/local address. */
void
tcp_conn_get_addr(uint32_t conn_id, ip4_addr_t *rip, uint16_t *rport,
                  ip4_addr_t *lip, uint16_t *lport)
{
    if (conn_id >= TCP_MAX_CONNS) return;
    tcp_conn_t *c = &s_tcp[conn_id];
    if (rip)   *rip   = c->remote_ip;
    if (rport) *rport = c->remote_port;
    if (lip)   *lip   = c->local_ip;
    if (lport) *lport = c->local_port;
}

/* tcp_conn_set_sock: update sock_id back-reference after accept. */
void
tcp_conn_set_sock(uint32_t conn_id, uint32_t sock_id)
{
    if (conn_id >= TCP_MAX_CONNS) return;
    s_tcp[conn_id].sock_id = sock_id;
}

/* tcp_conn_get: return pointer to tcp_conn_t for conn_id, or NULL if invalid. */
tcp_conn_t *
tcp_conn_get(uint32_t conn_id)
{
    if (conn_id >= TCP_MAX_CONNS) return (tcp_conn_t *)0;
    return &s_tcp[conn_id];
}
