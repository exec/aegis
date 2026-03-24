/* kernel/net/ip.c — IPv4 send/receive, ICMP echo, net_init */
#include "ip.h"
#include "eth.h"
#include "udp.h"
#include "tcp.h"
#include "../core/printk.h"
#include "../arch/x86_64/arch.h"   /* arch_get_ticks() */

/* Local memory helpers (kernel has no libc). */
static void _ip_memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void _ip_memcpy(void *dst, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    while (n--) *d++ = *s++;
}

/* ---- Checksum implementation ------------------------------------------ */

uint32_t net_checksum(const void *data, uint32_t len)
{
    const uint16_t *p   = (const uint16_t *)data;
    uint32_t        sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *(const uint8_t *)p;
    return sum;
}

uint16_t net_checksum_finish(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- IP configuration -------------------------------------------------- */

static ip4_addr_t s_my_ip;
static ip4_addr_t s_netmask;
static ip4_addr_t s_gateway;

void net_set_config(ip4_addr_t ip, ip4_addr_t mask, ip4_addr_t gw)
{
    s_my_ip   = ip;
    s_netmask = mask;
    s_gateway = gw;

    /* Print human-readable form: derive prefix length from mask. */
    uint32_t m  = ntohl(mask);
    int      pl = 0;
    while (m & 0x80000000u) { pl++; m <<= 1; }

    uint32_t a = ntohl(ip);
    uint32_t g = ntohl(gw);
    printk("[NET] configured: %u.%u.%u.%u/%u gw %u.%u.%u.%u\n",
           (a>>24)&0xff, (a>>16)&0xff, (a>>8)&0xff, a&0xff, pl,
           (g>>24)&0xff, (g>>16)&0xff, (g>>8)&0xff, g&0xff);
}

void net_get_config(ip4_addr_t *ip, ip4_addr_t *mask, ip4_addr_t *gw)
{
    if (ip)   *ip   = s_my_ip;
    if (mask) *mask = s_netmask;
    if (gw)   *gw   = s_gateway;
}

/* ---- IP send ----------------------------------------------------------- */

/* Static IP packet assembly buffer (1480 bytes payload max: 1500 MTU - 20 IP hdr). */
static uint8_t s_ip_buf[1500];
static uint16_t s_ip_id;

int ip_send(netdev_t *dev, ip4_addr_t dst_ip, uint8_t proto,
            const void *payload, uint16_t len)
{
    if (!dev)        return -1;
    if (len > 1480)  return -1; /* EMSGSIZE — no fragmentation in v1 */

    /* Determine next-hop MAC. */
    mac_addr_t next_hop_mac;

    if (dst_ip == htonl(0xFFFFFFFFu)) {
        /* Limited broadcast — no ARP needed. */
        _ip_memset(&next_hop_mac, 0xff, sizeof(next_hop_mac));
    } else {
        /* Same subnet? */
        ip4_addr_t via;
        if (s_my_ip == 0 || (dst_ip & s_netmask) == (s_my_ip & s_netmask))
            via = dst_ip;
        else
            via = s_gateway;

        if (arp_resolve(dev, via, &next_hop_mac) != 0)
            return -1; /* ARP timeout */
    }

    /* Build 20-byte IPv4 header. */
    ip_hdr_t *hdr   = (ip_hdr_t *)s_ip_buf;
    uint16_t  total = (uint16_t)(sizeof(ip_hdr_t) + len);

    hdr->ver_ihl    = 0x45;
    hdr->dscp_ecn   = 0;
    hdr->total_len  = htons(total);
    hdr->id         = htons(s_ip_id++);
    hdr->flags_frag = 0;
    hdr->ttl        = 64;
    hdr->proto      = proto;
    hdr->checksum   = 0;
    hdr->src        = s_my_ip;  /* may be 0.0.0.0 during DHCP bootstrap */
    hdr->dst        = dst_ip;
    hdr->checksum   = net_checksum_finish(net_checksum(hdr, sizeof(ip_hdr_t)));

    _ip_memcpy(s_ip_buf + sizeof(ip_hdr_t), payload, len);

    return eth_send(dev, &next_hop_mac, ETHERTYPE_IP, s_ip_buf, total);
}

/* ---- ICMP -------------------------------------------------------------- */

static uint8_t s_icmp_buf[1480];
/* Set to 1 by icmp_rx when an echo reply arrives; polled by net_init. */
static volatile int s_icmp_reply_received;

static void icmp_rx(netdev_t *dev, ip4_addr_t src_ip,
                    const icmp_hdr_t *icmp, uint16_t len)
{
    if (icmp->type == 0) {
        /* Echo reply received during net_init self-test.
         * Print the confirmation line — this is what test_net_stack.py checks. */
        uint32_t s = ntohl(src_ip);
        printk("[NET] ICMP: echo reply from %u.%u.%u.%u\n",
               (s>>24)&0xff, (s>>16)&0xff, (s>>8)&0xff, s&0xff);
        s_icmp_reply_received = 1;
        return;
    }

    if (icmp->type == 8) {
        /* Echo request — build reply: swap src/dst, type=0, recompute checksum. */
        if (len > (uint16_t)sizeof(s_icmp_buf)) return;
        _ip_memcpy(s_icmp_buf, icmp, len);
        icmp_hdr_t *reply = (icmp_hdr_t *)s_icmp_buf;
        reply->type     = 0;
        reply->checksum = 0;
        reply->checksum = net_checksum_finish(net_checksum(s_icmp_buf, len));
        ip_send(dev, src_ip, IP_PROTO_ICMP, s_icmp_buf, len);
    }
    /* All other ICMP types: drop silently. */
}

/* ---- IP receive -------------------------------------------------------- */

void ip_rx(netdev_t *dev, const void *frame,
           const void *ip_payload, uint16_t ip_payload_len)
{
    (void)frame; /* reserved for future gratuitous-ARP path */
    if (ip_payload_len < (uint16_t)sizeof(ip_hdr_t)) return;

    const ip_hdr_t *hdr = (const ip_hdr_t *)ip_payload;

    /* Validate version and header length. */
    if ((hdr->ver_ihl >> 4) != 4)   return;
    if ((hdr->ver_ihl & 0xf) != 5)  return; /* no IP options in v1 */

    /* Validate header checksum. */
    if (net_checksum_finish(net_checksum(hdr, sizeof(ip_hdr_t))) != 0) return;

    /* Destination filtering. */
    ip4_addr_t dst = hdr->dst;
    int accept = 0;
    if (dst == s_my_ip && s_my_ip != 0)                              accept = 1;
    if (dst == htonl(0xFFFFFFFFu))                                   accept = 1;
    if (s_my_ip == 0 && dst == htonl(0xFFFFFFFFu))                   accept = 1;
    if (s_my_ip != 0 && s_netmask != 0 &&
        dst == (s_my_ip | ~s_netmask))                               accept = 1;
    if (!accept) return;

    uint16_t hdr_len    = (uint16_t)sizeof(ip_hdr_t);
    uint16_t ip_tot_len = ntohs(hdr->total_len);
    if (ip_tot_len < hdr_len) return;               /* underflow guard */
    uint16_t data_len   = (uint16_t)(ip_tot_len - hdr_len);
    if (data_len > ip_payload_len - hdr_len) return;

    const void *proto_data = (const uint8_t *)ip_payload + hdr_len;

    switch (hdr->proto) {
    case IP_PROTO_ICMP:
        if (data_len >= (uint16_t)sizeof(icmp_hdr_t))
            icmp_rx(dev, hdr->src, (const icmp_hdr_t *)proto_data, data_len);
        break;
    case IP_PROTO_TCP:
        if (data_len >= 20u)   /* minimum TCP header is 20 bytes */
            tcp_rx(dev, hdr->src, hdr->dst, proto_data, data_len);
        break;
    case IP_PROTO_UDP:
        udp_rx(dev, hdr->src, hdr->dst, proto_data, data_len);
        break;
    default:
        break;
    }
}

/* ---- net_init ---------------------------------------------------------- */

static uint8_t s_ping_buf[sizeof(icmp_hdr_t)];

void net_init(void)
{
    netdev_t *dev = netdev_get("eth0");
    if (!dev) {
        /* No NIC registered (e.g. make test with -machine pc): silent return.
         * boot.txt must NOT contain any [NET] lines. */
        return;
    }

    eth_init();
    udp_init();
    tcp_init();

    /* Set static QEMU SLIRP IP configuration. */
    net_set_config(htonl(0x0a00020f), /* 10.0.2.15 */
                   htonl(0xffffff00), /* /24        */
                   htonl(0x0a000202)  /* 10.0.2.2   */);

    /* Send ICMP echo request to the QEMU SLIRP gateway (10.0.2.2). */
    icmp_hdr_t *req = (icmp_hdr_t *)s_ping_buf;
    req->type     = 8;
    req->code     = 0;
    req->id       = htons(0xAE91);
    req->seq      = htons(1);
    req->checksum = 0;
    req->checksum = net_checksum_finish(
                        net_checksum(s_ping_buf, sizeof(icmp_hdr_t)));

    int send_result = ip_send(dev, htonl(0x0a000202), IP_PROTO_ICMP,
                              s_ping_buf, sizeof(icmp_hdr_t));
    if (send_result != 0) {
        printk("[NET] WARN: ICMP send failed (ARP timeout?)\n");
        return;
    }
    printk("[NET] ICMP: echo request sent\n");

    /* Poll for the reply using sti+hlt+cli (same reason as arp_resolve).
     * net_init() runs before sched_start() with interrupts disabled, so
     * arch_get_ticks() never advances.  Yield to QEMU via sti+hlt+cli each
     * iteration; the PIT ISR calls netdev_poll_all() → icmp_rx() which
     * prints "[NET] ICMP: echo reply from ..." when the reply arrives.
     * 500 iterations × 10 ms/tick ≈ 5 seconds total timeout. */
    s_icmp_reply_received = 0;
    {
        uint32_t n;
        for (n = 0; n < 500u; n++) {
            __asm__ volatile("sti; hlt; cli");
            netdev_poll_all(); /* drain in case PIT ISR hasn't run yet */
            if (s_icmp_reply_received)
                break;
        }
    }
    __asm__ volatile("sti"); /* restore interrupts */
}
