/* kernel/net/eth.c — Ethernet framing, ARP table, ARP send/resolve */
#include "eth.h"
#include "ip.h"     /* ip_rx(), net_get_config() */
#include "../arch/x86_64/arch.h"   /* arch_get_ticks() */
#include "../core/printk.h"
#include <stddef.h>

/* Local memory helpers — kernel does not link against libc. */
static void
_eth_memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void
_eth_memcpy(void *dst, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    while (n--) *d++ = *s++;
}

/* ---- ARP table --------------------------------------------------------- */

#define ARP_TABLE_SIZE 16

typedef struct {
    ip4_addr_t ip;
    mac_addr_t mac;
    uint32_t   age;    /* PIT ticks since last use; evict oldest */
    uint8_t    valid;
} arp_entry_t;

static arp_entry_t s_arp_table[ARP_TABLE_SIZE];

/* Shared static TX buffer — callers are sequential (no concurrent sends). */
static uint8_t s_tx_buf[1514];

void eth_init(void)
{
    _eth_memset(s_arp_table, 0, sizeof(s_arp_table));
}

/* ---- ARP helpers ------------------------------------------------------- */

static arp_entry_t *arp_find(ip4_addr_t ip)
{
    int i;
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_arp_table[i].valid && s_arp_table[i].ip == ip)
            return &s_arp_table[i];
    }
    return NULL;
}

static void arp_insert(ip4_addr_t ip, const mac_addr_t *mac)
{
    /* Prefer an existing match to update, then a free slot, then oldest. */
    arp_entry_t *victim = NULL;
    uint32_t     oldest = 0;
    int          i;

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        arp_entry_t *e = &s_arp_table[i];
        if (e->valid && e->ip == ip) {
            victim = e;
            break;
        }
        if (!e->valid) {
            if (!victim) victim = e;
        } else if (e->age >= oldest) {
            oldest = e->age;
            if (!victim || victim->valid) victim = e;
        }
    }
    if (!victim) victim = &s_arp_table[0]; /* fallback — never NULL */
    victim->ip    = ip;
    victim->mac   = *mac;
    victim->age   = 0;
    victim->valid = 1;
}

static void arp_send_request(netdev_t *dev, ip4_addr_t target_ip)
{
    ip4_addr_t  my_ip;
    mac_addr_t  my_mac;
    arp_pkt_t   pkt;
    mac_addr_t  bcast;

    net_get_config(&my_ip, NULL, NULL);  /* only need local IP; mask/gw unused here */
    /* dev->mac is uint8_t[6]; copy into mac_addr_t (same layout). */
    _eth_memcpy(my_mac.b, dev->mac, 6);

    pkt.htype = htons(1);
    pkt.ptype = htons(ETHERTYPE_IP);
    pkt.hlen  = 6;
    pkt.plen  = 4;
    pkt.oper  = htons(1);       /* REQUEST */
    pkt.sha   = my_mac;
    pkt.spa   = my_ip;
    _eth_memset(&pkt.tha, 0, sizeof(pkt.tha));
    pkt.tpa   = target_ip;

    _eth_memset(bcast.b, 0xff, 6);
    eth_send(dev, &bcast, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

/* Called from eth_rx for ARP frames (inside and outside busy-poll). */
static void arp_rx_pkt(const arp_pkt_t *pkt)
{
    if (ntohs(pkt->htype) != 1)      return;
    if (ntohs(pkt->ptype) != 0x0800) return;
    if (pkt->hlen != 6 || pkt->plen != 4) return;
    if (ntohs(pkt->oper) != 2)       return;  /* only cache REPLY */
    arp_insert(pkt->spa, &pkt->sha);
}

/* ---- eth_rx / eth_send ------------------------------------------------- */

void eth_rx(netdev_t *dev, const void *frame, uint16_t len)
{
    const eth_hdr_t *hdr;
    const void      *payload;
    uint16_t         payload_len;
    uint16_t         et;

    if (len < (uint16_t)sizeof(eth_hdr_t)) return;
    hdr         = (const eth_hdr_t *)frame;
    payload     = (const uint8_t *)frame + sizeof(eth_hdr_t);
    payload_len = (uint16_t)(len - sizeof(eth_hdr_t));
    et          = ntohs(hdr->ethertype);

    if (et == ETHERTYPE_ARP && payload_len >= (uint16_t)sizeof(arp_pkt_t)) {
        arp_rx_pkt((const arp_pkt_t *)payload);
    } else if (et == ETHERTYPE_IP) {
        ip_rx(dev, frame, payload, payload_len);
    }
    /* Other ethertypes: drop silently */
}

int eth_send(netdev_t *dev, const mac_addr_t *dst_mac,
             uint16_t ethertype, const void *payload, uint16_t len)
{
    eth_hdr_t *hdr;
    uint16_t   total;
    mac_addr_t src_mac;

    if (!dev || len > 1500) return -1;

    hdr = (eth_hdr_t *)s_tx_buf;
    hdr->dst = *dst_mac;
    /* dev->mac is uint8_t[6]; copy into mac_addr_t src field. */
    _eth_memcpy(src_mac.b, dev->mac, 6);
    hdr->src       = src_mac;
    hdr->ethertype = htons(ethertype);
    _eth_memcpy(s_tx_buf + sizeof(eth_hdr_t), payload, len);

    total = (uint16_t)(sizeof(eth_hdr_t) + len);
    dev->send(dev, s_tx_buf, total);
    return 0;
}

/* ---- arp_resolve ------------------------------------------------------- */

int arp_resolve(netdev_t *dev, ip4_addr_t ip, mac_addr_t *mac_out)
{
    arp_entry_t *e = arp_find(ip);
    if (e) {
        e->age = 0;
        *mac_out = e->mac;
        return 0;
    }

    /* Not cached — send ARP REQUEST and poll using sti+hlt+cli.
     *
     * net_init() is called before sched_start(), so interrupts are disabled
     * (IF=0) and arch_get_ticks() never advances.  We must NOT use a
     * tick-based timeout.
     *
     * On TCG (no KVM) QEMU, the guest vCPU and QEMU's SLIRP/virtio backend
     * share a single host thread.  To let SLIRP generate the ARP reply we
     * must give QEMU real CPU time.  The pattern "sti; hlt; cli" achieves
     * this: sti unmasks the PIT IRQ, hlt yields until the next PIT interrupt
     * (10 ms), the PIT ISR runs (and calls netdev_poll_all() → virtio_net_poll
     * → delivers any pending RX frames including the ARP reply), then cli
     * re-masks interrupts for the table check.
     *
     * Concurrency: the PIT ISR may deliver the ARP reply via netdev_poll_all()
     * before we call arp_find() here.  That is fine — arp_insert() is
     * idempotent and arp_find() will see the cached entry on the next check.
     *
     * Timeout: 500 iterations × 10 ms/tick ≈ 5 seconds.  More than enough
     * for SLIRP on any host.
     */
    arp_send_request(dev, ip);

    {
        uint32_t n;
        for (n = 0; n < 500u; n++) {
            /* Yield to QEMU/SLIRP: enable interrupts, halt until PIT fires,
             * re-disable.  The PIT ISR calls netdev_poll_all() which drains
             * the virtio RX ring and delivers any pending frames. */
            arch_wait_for_irq();
            /* Also poll directly in case PIT ISR didn't run yet. */
            if (dev->poll)
                dev->poll(dev);
            e = arp_find(ip);
            if (e) {
                arch_enable_irq(); /* restore interrupts */
                *mac_out = e->mac;
                return 0;
            }
        }
    }
    arch_enable_irq(); /* restore interrupts on timeout path */
    return -1; /* timeout — caller returns EHOSTUNREACH */
}
