/* kernel/net/tcp.h — TCP connection table, state machine, retransmit */
#ifndef TCP_H
#define TCP_H

#include "net.h"
#include "netdev.h"

#define TCP_MAX_CONNS  32
#define TCP_RBUF_SIZE  16384
#define TCP_SBUF_SIZE  8192

/* TIME_WAIT duration: 4 seconds (shortened 2MSL; non-production acceptable). */
#define TCP_TIMEWAIT_TICKS  400   /* 4 s at 100 Hz */
/* Retransmit timeout: 1 s initial, doubles to 8 s (3 retransmits then RST). */
#define TCP_RTO_INITIAL     100   /* 1 s at 100 Hz */
#define TCP_RTO_MAX         800   /* 8 s at 100 Hz */
#define TCP_RETRANSMIT_MAX  3

typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

typedef struct {
    tcp_state_t state;
    ip4_addr_t  local_ip,  remote_ip;
    uint16_t    local_port, remote_port;
    netdev_t   *dev;           /* NIC this connection arrived on */
    uint32_t    snd_nxt, snd_una;
    uint32_t    rcv_nxt;
    uint16_t    snd_wnd;
    uint8_t     rbuf[TCP_RBUF_SIZE];
    uint32_t    rbuf_head, rbuf_tail;
    uint8_t     sbuf[TCP_SBUF_SIZE];
    uint32_t    sbuf_head, sbuf_tail;
    uint32_t    retransmit_at;
    uint8_t     retransmit_count;
    uint32_t    timewait_at;
    uint32_t    sock_id;
    uint32_t    listener_id;
} tcp_conn_t;

/* tcp_init: zero the connection table. Called from net_init(). */
void tcp_init(void);

/* tcp_rx: process an inbound TCP segment. Called by ip_rx for IP_PROTO_TCP. */
void tcp_rx(netdev_t *dev, ip4_addr_t src_ip, ip4_addr_t dst_ip,
            const void *tcp_data, uint16_t len);

/* tcp_tick: retransmit timer tick. Called from the PIT handler at 100 Hz. */
void tcp_tick(void);

/* tcp_send_segment: build a 20-byte TCP header + optional payload and call ip_send. */
int tcp_send_segment(netdev_t *dev, tcp_conn_t *conn,
                     uint8_t flags, const void *payload, uint16_t len);

/* ── Socket-layer helpers (Phase 26) ─────────────────────────────────────── */
int  tcp_listen(uint16_t port, uint32_t sock_id);
int  tcp_connect(uint32_t sock_id, ip4_addr_t dst_ip, uint16_t dst_port,
                 uint32_t *conn_id_out);
int  tcp_conn_recv(uint32_t conn_id, void *dst, uint16_t max_len);
int  tcp_conn_send(uint32_t conn_id, const void *data, uint16_t len);
int  tcp_conn_close(uint32_t conn_id);
void tcp_conn_get_addr(uint32_t conn_id, ip4_addr_t *rip, uint16_t *rport,
                       ip4_addr_t *lip, uint16_t *lport);
void tcp_conn_set_sock(uint32_t conn_id, uint32_t sock_id);
/* tcp_conn_get: return pointer to tcp_conn_t for conn_id, or NULL if invalid. */
tcp_conn_t *tcp_conn_get(uint32_t conn_id);

#endif /* TCP_H */
