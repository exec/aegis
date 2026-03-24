/* kernel/net/tcp.c — TCP state machine (RFC 793), retransmit, TIME_WAIT */
#include "tcp.h"
#include "ip.h"
#include "../arch/x86_64/arch.h"   /* arch_get_ticks() */
#include "../core/printk.h"
#include <stddef.h>                 /* NULL */

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
            tcp_conn_t *listener = tcp_find_listener(dst_ip, local_port);
            if (!listener) {
                tcp_conn_t tmp;
                _tcp_memset(&tmp, 0, sizeof(tmp));
                tmp.local_ip    = dst_ip;
                tmp.local_port  = local_port;
                tmp.remote_ip   = src_ip;
                tmp.remote_port = remote_port;
                tmp.snd_nxt     = 0;
                tmp.rcv_nxt     = seq + 1;
                tcp_send_segment(dev, &tmp, TCP_RST | TCP_ACK, NULL, 0);
                return;
            }
            conn = tcp_alloc();
            if (!conn) return;
            conn->state       = TCP_SYN_RCVD;
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
                conn->retransmit_at = 0;
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
            }
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_ACK) {
            if (ack > conn->snd_una)
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
            }
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_segment(dev, conn, TCP_ACK, NULL, 0);
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
            if (now >= c->timewait_at)
                c->state = TCP_CLOSED;
            continue;
        }

        if (c->retransmit_at == 0) continue;
        if (now < c->retransmit_at) continue;

        if (c->retransmit_count >= TCP_RETRANSMIT_MAX) {
            /* Maximum retransmits — RST best-effort (NULL dev) and close. */
            tcp_send_segment(NULL, c, TCP_RST, NULL, 0);
            c->state = TCP_CLOSED;
            continue;
        }

        c->retransmit_count++;
        uint32_t rto = TCP_RTO_INITIAL << c->retransmit_count;
        if (rto > TCP_RTO_MAX) rto = TCP_RTO_MAX;
        c->retransmit_at = now + rto;

        if (c->state == TCP_SYN_RCVD)
            tcp_send_segment(NULL, c, TCP_SYN | TCP_ACK, NULL, 0);
        else if (c->state == TCP_SYN_SENT)
            tcp_send_segment(NULL, c, TCP_SYN, NULL, 0);
    }
}
