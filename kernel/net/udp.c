/* kernel/net/udp.c — UDP send/receive, port demultiplexing */
#include "udp.h"
#include "ip.h"

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

void udp_rx(netdev_t *dev, ip4_addr_t src_ip, ip4_addr_t dst_ip,
            const void *udp_data, uint16_t len)
{
    (void)dev; (void)src_ip; (void)dst_ip;
    if (len < (uint16_t)sizeof(udp_hdr_t)) return;

    const udp_hdr_t *hdr      = (const udp_hdr_t *)udp_data;
    uint16_t         dst_port = ntohs(hdr->dst_port);
    int i;

    for (i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (s_udp[i].port == dst_port && s_udp[i].port != 0) {
            /* Socket delivery deferred to Phase 26. */
            return;
        }
    }
    /* No binding: drop silently. */
}
