/* kernel/net/udp.c — UDP send/receive, port demultiplexing */
#include "udp.h"
#include "ip.h"
#include "socket.h"
#include "epoll.h"
#include "spinlock.h"

/* Local memory helpers. */
static void _udp_memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void _udp_memcpy(void *dst, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    while (n--) *d++ = *s++;
}

/* ---- Binding table ----------------------------------------------------- */

typedef struct {
    uint16_t port;      /* host byte order; 0 = free */
    uint32_t sock_id;   /* index into socket table (Phase 26) */
} udp_binding_t;

static udp_binding_t s_udp[UDP_BINDINGS_MAX];
static spinlock_t udp_lock = SPINLOCK_INIT;

void udp_init(void)
{
    _udp_memset(s_udp, 0, sizeof(s_udp));
}

/* Static UDP packet assembly buffer. */
static uint8_t s_udp_buf[1480];

int udp_send(netdev_t *dev, uint16_t src_port, ip4_addr_t dst_ip,
             uint16_t dst_port, const void *payload, uint16_t len)
{
    if (!dev) return -1;
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + len);
    if (udp_len > (uint16_t)sizeof(s_udp_buf)) return -1;

    udp_hdr_t *hdr = (udp_hdr_t *)s_udp_buf;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons(udp_len);
    hdr->checksum = 0;
    _udp_memcpy(s_udp_buf + sizeof(udp_hdr_t), payload, len);

    return ip_send(dev, dst_ip, IP_PROTO_UDP, s_udp_buf, udp_len);
}

/* S4: UDP pseudo-header checksum validation.
 * Computes one's-complement sum over pseudo-header + UDP header + payload.
 * Returns 0 if valid, non-zero if corrupted. */
static uint16_t
udp_checksum_verify(uint32_t src_ip, uint32_t dst_ip,
                    const uint8_t *udp_pkt, uint16_t udp_len)
{
    uint32_t sum = 0;
    /* pseudo-header */
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += htons(17);            /* protocol = UDP */
    sum += htons(udp_len);
    /* UDP header + payload */
    const uint16_t *p = (const uint16_t *)udp_pkt;
    uint16_t i;
    for (i = 0; i < udp_len / 2; i++)
        sum += p[i];
    if (udp_len & 1)
        sum += udp_pkt[udp_len - 1];  /* odd byte, zero-padded */
    /* fold carries */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

void udp_rx(netdev_t *dev, ip4_addr_t src_ip, ip4_addr_t dst_ip,
            const void *udp_data, uint16_t len)
{
    (void)dev;
    if (len < (uint16_t)sizeof(udp_hdr_t)) return;
    irqflags_t fl = spin_lock_irqsave(&udp_lock);

    /* Deferred socket wake/notify — avoids udp_lock → sock_lock inversion. */
    uint32_t wake_sid = SOCK_NONE;

    const udp_hdr_t *hdr      = (const udp_hdr_t *)udp_data;
    uint16_t udp_claimed = ntohs(hdr->length);
    if (udp_claimed < (uint16_t)sizeof(udp_hdr_t)) goto udp_out;  /* malformed */
    if (udp_claimed > len) goto udp_out;                           /* truncated */

    /* S4: Validate UDP checksum (skip if checksum field is 0 per RFC 768). */
    if (hdr->checksum != 0) {
        if (udp_checksum_verify(src_ip, dst_ip,
                                (const uint8_t *)hdr, udp_claimed) != 0)
            goto udp_out;  /* drop corrupted packet */
    }

    {
    uint16_t dst_port  = ntohs(hdr->dst_port);
    uint16_t src_port  = ntohs(hdr->src_port);
    uint16_t payload_len = (uint16_t)(udp_claimed - (uint16_t)sizeof(udp_hdr_t));
    const uint8_t *payload = (const uint8_t *)udp_data + sizeof(udp_hdr_t);
    int i;

    for (i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (s_udp[i].port == dst_port && s_udp[i].port != 0) {
            uint32_t sid = s_udp[i].sock_id;
            if (sid == SOCK_NONE) goto udp_out;
            sock_t *s = sock_get_nolock(sid);
            if (!s) goto udp_out;
            /* Find a free UDP RX slot */
            uint8_t next = (uint8_t)((s->udp_rx_tail + 1) & (UDP_RX_SLOTS - 1));
            if (next == s->udp_rx_head) goto udp_out;  /* ring full, drop */
            udp_rx_slot_t *slot = &s->udp_rx[s->udp_rx_tail];
            if (payload_len > UDP_RX_MAXBUF) payload_len = UDP_RX_MAXBUF;
            uint32_t j;
            for (j = 0; j < payload_len; j++)
                slot->data[j] = payload[j];
            slot->len      = payload_len;
            slot->src_ip   = src_ip;
            slot->src_port = src_port;
            slot->in_use   = 1;
            s->udp_rx_tail = next;
            wake_sid = sid;
            goto udp_out;
        }
    }
    }
    /* No binding: drop silently. */
udp_out:
    spin_unlock_irqrestore(&udp_lock, fl);

    /* Wake + epoll notify outside udp_lock (sock_lock > udp_lock). */
    if (wake_sid != SOCK_NONE) {
        sock_wake(wake_sid);
        epoll_notify(wake_sid, EPOLLIN);
        /* Wake any sys_poll / sys_epoll_wait waiter on this socket. */
        {
            sock_t *s_wake = sock_get(wake_sid);
            if (s_wake) waitq_wake_all(&s_wake->poll_waiters);
        }
    }
}

/* udp_bind: register a sock_id for the given port. Returns 0 or -1. */
int
udp_bind(uint16_t port, uint32_t sock_id)
{
    irqflags_t fl = spin_lock_irqsave(&udp_lock);
    uint32_t i;
    for (i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (s_udp[i].port == port && s_udp[i].port != 0) {
            spin_unlock_irqrestore(&udp_lock, fl);
            return -1; /* EADDRINUSE */
        }
    }
    for (i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (s_udp[i].port == 0) {
            s_udp[i].port    = port;
            s_udp[i].sock_id = sock_id;
            spin_unlock_irqrestore(&udp_lock, fl);
            return 0;
        }
    }
    spin_unlock_irqrestore(&udp_lock, fl);
    return -1;
}

/* udp_unbind: release the binding table entry for `port`.
 * Called from sock_vfs_close so a subsequent bind() to the same port
 * doesn't fail with EADDRINUSE. Without this, every UDP socket that
 * gets closed leaks its port binding (DHCP retry was the symptom that
 * exposed it: second bind to port 68 failed). */
void
udp_unbind(uint16_t port)
{
    if (port == 0) return;
    irqflags_t fl = spin_lock_irqsave(&udp_lock);
    uint32_t i;
    for (i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (s_udp[i].port == port) {
            s_udp[i].port    = 0;
            s_udp[i].sock_id = 0;
        }
    }
    spin_unlock_irqrestore(&udp_lock, fl);
}
