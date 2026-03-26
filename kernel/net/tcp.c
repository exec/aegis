/* kernel/net/tcp.c — TCP state machine (RFC 793), retransmit, TIME_WAIT */
#include "tcp.h"
#include "ip.h"
#include "socket.h"
#include "epoll.h"
#include "../arch/x86_64/arch.h"   /* arch_get_ticks() */
#include "../core/printk.h"
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
    hdr->window   = htons(4096);
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
    const tcp_hdr_t *seg = (const tcp_hdr_t *)tcp_data;

    tcp_pseudo_hdr_t ph;
    ph.src     = src_ip;
    ph.dst     = dst_ip;
    ph.zero    = 0;
    ph.proto   = IP_PROTO_TCP;
    ph.tcp_len = htons(len);
    uint32_t sum = 0;
    sum += net_checksum(&ph, sizeof(ph));
    sum += net_checksum(tcp_data, len);
    if (net_checksum_finish(sum) != 0) return;

    uint16_t remote_port = ntohs(seg->src_port);
    uint16_t local_port  = ntohs(seg->dst_port);
    uint8_t  hdr_bytes   = (uint8_t)((seg->data_off >> 4) * 4);
    if (hdr_bytes < sizeof(tcp_hdr_t) || hdr_bytes > len) return;
    uint16_t payload_len = (uint16_t)(len - hdr_bytes);
    const uint8_t *payload = (const uint8_t *)tcp_data + hdr_bytes;
    uint32_t seq = ntohl(seg->seq);
    uint32_t ack = ntohl(seg->ack);
    uint8_t  flags = seg->flags;

    tcp_conn_t *conn = tcp_find(src_ip, remote_port, dst_ip, local_port);

    if (!conn) {
        if (flags & TCP_SYN) {
            printk("[TCP] SYN dst_port=%u\n", (uint32_t)local_port);
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
                return;
            }
            conn = tcp_alloc();
            if (!conn) return;
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
        return;
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
                /* Wake accept() waiter on the listener socket */
                if (conn->listener_id != SOCK_NONE) {
                    sock_t *ls = sock_get(conn->listener_id);
                    if (ls) {
                        uint8_t next_tail = (uint8_t)((ls->accept_tail + 1) & 7);
                        if (next_tail != ls->accept_head) {
                            uint32_t conn_id = (uint32_t)(conn - s_tcp);
                            ls->accept_queue[ls->accept_tail] = conn_id;
                            ls->accept_tail = next_tail;
                        }
                        sock_wake(conn->listener_id);
                        epoll_notify(conn->listener_id, EPOLLIN);
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
                /* Wake connect() waiter on this socket */
                if (conn->sock_id != SOCK_NONE) {
                    sock_wake(conn->sock_id);
                    epoll_notify(conn->sock_id, EPOLLOUT);
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
                /* Wake blocked recv() on this socket */
                if (conn->sock_id != SOCK_NONE) {
                    sock_wake(conn->sock_id);
                    epoll_notify(conn->sock_id, EPOLLIN);
                }
            }
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
            /* Wake blocked recv() — EOF/hangup */
            if (conn->sock_id != SOCK_NONE) {
                sock_wake(conn->sock_id);
                epoll_notify(conn->sock_id, EPOLLHUP);
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
            if (conn->sock_id != SOCK_NONE) {
                sock_wake(conn->sock_id);
                epoll_notify(conn->sock_id, EPOLLHUP);
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
}

void tcp_tick(void)
{
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
}

/* ── Socket-layer helpers (Phase 26) ─────────────────────────────────────── */

/* tcp_listen: register a listening socket at port. */
int
tcp_listen(uint16_t port, uint32_t sock_id)
{
    uint32_t i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (s_tcp[i].state == TCP_CLOSED) {
            _tcp_memset(&s_tcp[i], 0, sizeof(s_tcp[i]));
            s_tcp[i].state      = TCP_LISTEN;
            s_tcp[i].local_port = port;
            s_tcp[i].sock_id    = sock_id;
            return 0;
        }
    }
    return -1;
}

/* tcp_connect: send SYN, allocate conn. Returns 0 on success. */
int
tcp_connect(uint32_t sock_id, ip4_addr_t dst_ip, uint16_t dst_port,
            uint32_t *conn_id_out)
{
    extern netdev_t *netdev_get(const char *name);
    uint32_t i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (s_tcp[i].state == TCP_CLOSED) {
            _tcp_memset(&s_tcp[i], 0, sizeof(s_tcp[i]));
            s_tcp[i].state       = TCP_SYN_SENT;
            s_tcp[i].local_port  = (uint16_t)(49152u + (arch_get_ticks() & 0x3FFFu));
            s_tcp[i].remote_ip   = dst_ip;
            s_tcp[i].remote_port = dst_port;
            s_tcp[i].sock_id     = sock_id;
            s_tcp[i].snd_nxt     = (uint32_t)arch_get_ticks();
            s_tcp[i].snd_una     = s_tcp[i].snd_nxt;
            *conn_id_out = i;
            netdev_t *dev = netdev_get("eth0");
            s_tcp[i].dev = dev;
            tcp_send_segment(dev, &s_tcp[i], TCP_SYN, (void *)0, 0);
            s_tcp[i].snd_nxt++;
            s_tcp[i].retransmit_at = (uint32_t)arch_get_ticks() + TCP_RTO_INITIAL;
            return 0;
        }
    }
    return -1;
}

/* tcp_conn_recv: read up to max_len bytes from rbuf. max_len=0 returns available count. */
int
tcp_conn_recv(uint32_t conn_id, void *dst, uint16_t max_len)
{
    if (conn_id >= TCP_MAX_CONNS) return -1;
    tcp_conn_t *c = &s_tcp[conn_id];
    uint32_t avail = (c->rbuf_tail - c->rbuf_head + TCP_RBUF_SIZE) % TCP_RBUF_SIZE;
    if (max_len == 0) return (int)avail;  /* peek */
    if (avail == 0) {
        if (c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED) return 0;  /* EOF */
        return -11;  /* EAGAIN */
    }
    uint32_t n = avail < max_len ? avail : max_len;
    uint8_t *d = (uint8_t *)dst;
    uint32_t j;
    for (j = 0; j < n; j++) {
        d[j] = c->rbuf[c->rbuf_head];
        c->rbuf_head = (c->rbuf_head + 1) % TCP_RBUF_SIZE;
    }
    return (int)n;
}

/* tcp_conn_send: copy data into sbuf and send. */
int
tcp_conn_send(uint32_t conn_id, const void *data, uint16_t len)
{
    if (conn_id >= TCP_MAX_CONNS) return -1;
    tcp_conn_t *c = &s_tcp[conn_id];
    if (c->state != TCP_ESTABLISHED) return -32;  /* EPIPE */
    return tcp_send_segment(c->dev, c, TCP_PSH | TCP_ACK, data, len) == 0 ? len : -1;
}

/* tcp_conn_close: send FIN. */
int
tcp_conn_close(uint32_t conn_id)
{
    if (conn_id >= TCP_MAX_CONNS) return -1;
    tcp_conn_t *c = &s_tcp[conn_id];
    if (c->state == TCP_ESTABLISHED) {
        tcp_send_segment(c->dev, c, TCP_FIN | TCP_ACK, (void *)0, 0);
        c->state = TCP_FIN_WAIT_1;
        c->snd_nxt++;
    }
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
