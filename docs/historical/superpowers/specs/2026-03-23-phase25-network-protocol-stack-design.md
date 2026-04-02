# Phase 25: Ethernet/ARP/IP/ICMP/TCP/UDP Protocol Stack Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a complete in-kernel TCP/IP protocol stack on top of `netdev_t`, supporting Ethernet, ARP, IPv4, ICMP echo, UDP, and TCP with a full state machine.

**Architecture:** Layered receive dispatch: `netdev_rx_deliver` → `eth_rx` → `ip_rx` → TCP/UDP/ICMP handlers. Send path is symmetric bottom-up. All protocol state is in static kernel tables (no heap). Copy-based packet path (no zero-copy in v1).

**Tech Stack:** C, static tables, PIT-based retransmit timers, `sched_block`/`sched_wake` for blocking operations.

---

## Constraints and Non-Negotiables

- No dynamic allocation. All connection state, packet buffers, and ARP entries are in statically-sized arrays.
- Maximum simultaneous TCP connections: 32. Maximum UDP sockets: 16. ARP table: 16 entries (LRU eviction).
- No IP fragmentation/reassembly in v1. Sends are capped at MTU (1500 bytes). Oversized sends return `EMSGSIZE`.
- No congestion control in v1. Fixed 4KB send window per TCP connection.
- `TIME_WAIT` duration: 4 seconds (shortened from 2MSL; acceptable for non-production use).
- Retransmit timeout: 1 second initial, doubles up to 8 seconds (3 retransmits then RST).
- All checksum computation is software-only. No hardware offload.
- IP configuration (address, mask, gateway) is stored in `kernel/net/ip.c` static globals, set by `net_set_config()`. Initially `0.0.0.0` (unconfigured). The DHCP daemon (Phase 28) calls `sys_netcfg` to set this.
- `make test` uses `-machine pc` — no NIC. Protocol stack initializes its tables silently. No printk from `eth_init`, `ip_init`, `tcp_init`, `udp_init` unless a NIC is registered.

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `kernel/net/net.h` | Create | Shared types: `ip4_addr_t`, `mac_addr_t`, checksum utils, byte-order macros |
| `kernel/net/eth.h` + `eth.c` | Create | Ethernet framing, ARP table, ARP send/resolve |
| `kernel/net/ip.h` + `ip.c` | Create | IPv4 send/receive, routing, `net_set_config()`, ICMP echo |
| `kernel/net/udp.h` + `udp.c` | Create | UDP send/receive, port demux to socket layer |
| `kernel/net/tcp.h` + `tcp.c` | Create | TCP state machine, send/receive ring buffers, retransmit |
| `kernel/net/netdev.c` | Modify | Replace stub `netdev_rx_deliver` with `eth_rx()` call |
| `kernel/core/main.c` | Modify | Call `net_init()` after `virtio_net_init()` |
| `Makefile` | Modify | Add net stack sources to NET_SRCS |
| `tests/test_net_stack.py` | Create | Boot q35 + virtio-net + SLIRP, send ICMP ping, verify reply via serial |
| `tests/run_tests.sh` | Modify | Add `test_net_stack.py` |

---

## Shared Types (`kernel/net/net.h`)

```c
typedef uint32_t ip4_addr_t;   /* network byte order */
typedef struct { uint8_t b[6]; } mac_addr_t;

#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)

/* net_checksum: accumulate a running 32-bit one's complement sum over `len` bytes.
 * Returns a uint32_t partial sum (NOT folded, NOT inverted).
 * Call multiple times for non-contiguous regions (TCP/UDP pseudo-header + payload).
 * uint32_t sum = 0;
 * sum += net_checksum(pseudo_header, sizeof(pseudo_header));
 * sum += net_checksum(payload, payload_len);
 * header->checksum = net_checksum_finish(sum);
 */
uint32_t net_checksum(const void *data, uint32_t len);

/* net_checksum_finish: fold carry bits into 16 bits, invert (one's complement).
 * Returns the final 16-bit checksum value ready to store in the header.
 * A checksum of 0x0000 after folding becomes 0xFFFF per RFC (UDP, not TCP). */
uint16_t net_checksum_finish(uint32_t sum);
```

---

## Ethernet Layer (`eth.c`)

### Frame Format (14-byte header)
```
dst_mac[6] | src_mac[6] | ethertype[2]
```
Ethertype: `0x0800` = IPv4, `0x0806` = ARP.

### ARP Table

```c
#define ARP_TABLE_SIZE 16
typedef struct {
    ip4_addr_t ip;
    mac_addr_t mac;
    uint32_t   age;   /* PIT ticks since last use; evict oldest */
    uint8_t    valid;
} arp_entry_t;
static arp_entry_t s_arp_table[ARP_TABLE_SIZE];
```

### `arp_resolve(ip, mac_out)`

1. Search table for matching IP → return cached MAC if found.
2. If not found: send ARP REQUEST (broadcast), busy-poll the NIC RX used ring for up to 1000 PIT ticks (~100ms). **Disable interrupts (`cli`) for the duration of the busy-poll** — this prevents the PIT ISR from concurrently calling `virtio_net_poll()` and advancing `rx_last_used` while the busy-poll is doing the same, which would cause frames to be consumed twice or silently dropped. Re-enable interrupts (`sti`) when the busy-poll exits (whether on ARP reply, timeout, or error). **During this poll, only ARP frames are processed** — all other frame types (IP, etc.) are silently discarded. This prevents re-entrant TCP/UDP state machine processing while the caller is already in a TCP connect path. If ARP REPLY received, cache and return. If timeout, return -1 (caller returns `EHOSTUNREACH`).
3. ARP table entries are updated on any ARP REPLY seen outside the busy-poll path (gratuitous ARP support for free).

### `eth_send(dev, dst_mac, ethertype, payload, len)`

Builds 14-byte Ethernet header + payload into a static 1514-byte TX buffer, calls `dev->send()`.

### `eth_rx(dev, frame, len)`

Dispatches on ethertype: ARP → `arp_rx()`, IPv4 → `ip_rx()`, other → drop.

---

## IP Layer (`ip.c`)

### IP Configuration

```c
static ip4_addr_t s_my_ip;      /* 0 = unconfigured */
static ip4_addr_t s_netmask;
static ip4_addr_t s_gateway;

void net_set_config(ip4_addr_t ip, ip4_addr_t mask, ip4_addr_t gw);
void net_get_config(ip4_addr_t *ip, ip4_addr_t *mask, ip4_addr_t *gw);
```

### `ip_send(dst_ip, proto, payload, len)`

1. If `dst_ip == 0xFFFFFFFF` (255.255.255.255) → use broadcast MAC `ff:ff:ff:ff:ff:ff` directly, no ARP. Required for DHCP DISCOVER/REQUEST.
2. If `dst_ip` is on local subnet → `arp_resolve(dst_ip)` for next-hop MAC.
3. Otherwise → `arp_resolve(s_gateway)` for gateway MAC.
4. Build 20-byte IP header (version=4, IHL=5, TTL=64, `src_ip = s_my_ip` which may be `0.0.0.0` during DHCP bootstrap — `ip_send` must not reject sends with `src_ip == 0`). Compute header checksum.
5. Call `eth_send(dev, next_hop_mac, 0x0800, ip_pkt, ip_len)`.

### `ip_rx(dev, frame, ip_hdr, len)`

1. Validate header checksum; drop on mismatch.
2. Accept frame if `dst_ip == s_my_ip` OR `dst_ip == 0xFFFFFFFF` (limited broadcast) OR `dst_ip == (s_my_ip | ~s_netmask)` (subnet broadcast). Also accept if `s_my_ip == 0` and `dst_ip == 0xFFFFFFFF` — this permits DHCP OFFER/ACK reception before an IP address is assigned. Drop all other destination IPs.
3. Dispatch on protocol: `1` = ICMP → `icmp_rx()`, `6` = TCP → `tcp_rx()`, `17` = UDP → `udp_rx()`.

### ICMP Echo

`icmp_rx()` handles type 8 (echo request) only — swaps src/dst IP, sets type to 0 (echo reply), recomputes checksum, calls `ip_send()`. All other ICMP types are dropped.

---

## UDP Layer (`udp.c`)

### Demux

```c
#define UDP_BINDINGS_MAX 16
typedef struct {
    uint16_t   port;      /* host byte order; 0 = free */
    uint32_t   sock_id;   /* index into socket table (Phase 26) */
} udp_binding_t;
static udp_binding_t s_udp[UDP_BINDINGS_MAX];
```

`udp_rx()` looks up `dst_port` in the binding table and delivers the payload to the socket's receive ring. If no binding found, sends ICMP port-unreachable (optional, can be a drop).

### `udp_send(src_port, dst_ip, dst_port, payload, len)`

Builds 8-byte UDP header, calls `ip_send(dst_ip, 17, udp_pkt, udp_len)`. Checksum is optional for IPv4 UDP (set to 0).

---

## TCP Layer (`tcp.c`)

### Connection Table

```c
#define TCP_MAX_CONNS 32
#define TCP_RBUF_SIZE 8192
#define TCP_SBUF_SIZE 8192

typedef enum {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_RCVD, TCP_SYN_SENT,
    TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2,
    TCP_CLOSING,    /* simultaneous close: FIN_WAIT_1 + peer FIN → CLOSING + ACK → TIME_WAIT */
    TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT
} tcp_state_t;

typedef struct {
    tcp_state_t state;
    ip4_addr_t  local_ip, remote_ip;
    uint16_t    local_port, remote_port;
    uint32_t    snd_nxt, snd_una;   /* send sequence numbers */
    uint32_t    rcv_nxt;            /* receive sequence number */
    uint16_t    snd_wnd;            /* send window (peer's advertised window) */
    uint8_t     rbuf[TCP_RBUF_SIZE];
    uint32_t    rbuf_head, rbuf_tail;
    uint8_t     sbuf[TCP_SBUF_SIZE];
    uint32_t    sbuf_head, sbuf_tail;
    uint32_t    retransmit_at;      /* PIT tick for next retransmit */
    uint8_t     retransmit_count;
    uint32_t    timewait_at;        /* PIT tick when TIME_WAIT expires */
    uint32_t    sock_id;            /* owning socket (Phase 26) */
    uint32_t    listener_id;        /* parent LISTEN socket for SYN_RCVD */
} tcp_conn_t;

static tcp_conn_t s_tcp[TCP_MAX_CONNS];
```

### State Machine

`tcp_rx()` implements the full RFC 793 state machine. Key transitions:
- LISTEN + SYN → SYN_RCVD: send SYN+ACK, allocate connection slot
- SYN_RCVD + ACK → ESTABLISHED: wake `accept()` waiter
- ESTABLISHED + data → buffer in rbuf, send ACK, wake `recv()` waiter
- ESTABLISHED + FIN → CLOSE_WAIT: send ACK, notify socket of EOF
- CLOSE_WAIT → LAST_ACK on `close()`: send FIN
- FIN_WAIT_1 + ACK → FIN_WAIT_2
- FIN_WAIT_1 + FIN → CLOSING: send ACK (simultaneous close)
- CLOSING + ACK → TIME_WAIT: set `timewait_at`
- FIN_WAIT_2 + FIN → TIME_WAIT: send ACK, set `timewait_at`
- TIME_WAIT expires → CLOSED (slot free)

### Retransmit

`tcp_tick()` is called from PIT handler (separate from `virtio_net_poll`). For each ESTABLISHED/FIN_WAIT connection: if `snd_una < snd_nxt` and `arch_get_ticks() >= retransmit_at`, retransmit oldest unacknowledged segment. Double timeout, increment count. At count 3, send RST and close.

### `tcp_send_segment(conn, flags, payload, len)`

Builds TCP header (20 bytes, no options), pseudo-header checksum, calls `ip_send()`.

---

## Testing

### `tests/test_net_stack.py`

Boots with `-machine q35 -device virtio-net-pci,disable-legacy=on -netdev user,id=n0`.

The kernel sends an ICMP ping to QEMU's gateway (`10.0.2.2`) during `net_init()` as a self-test. QEMU user networking responds to ICMP natively. The test verifies:
```python
assert "[NET] ICMP: echo reply from 10.0.2.2" in output
```

The static test IP `10.0.2.15/24` is hardcoded in the kernel for Phase 25 only. Phase 28 (DHCP) replaces this with dynamic configuration.

---

## Forward-Looking Constraints

**Static IP in Phase 25.** `net_init()` sets `s_my_ip = 10.0.2.15`, `s_netmask = 255.255.255.0`, `s_gateway = 10.0.2.2` for QEMU SLIRP compatibility. This hardcoded assignment must be removed (or guarded with a build flag) when Phase 28 is integrated — the DHCP daemon will configure the IP dynamically via `sys_netcfg`.

**`eth_send` uses a file-level static TX buffer.** It is a single 1514-byte `static uint8_t s_tx_buf[]` in `eth.c`. Callers are sequential (no concurrent sends). When `arp_resolve` is called from within `ip_send` (e.g. to resolve the next-hop MAC for a TCP SYN), `arp_resolve` calls `eth_send` to transmit the ARP REQUEST, waits for the ARP REPLY, then **returns** — only then does `ip_send` call `eth_send` for the original IP packet. These two uses of `s_tx_buf` are sequential, not nested; the buffer is not in use when the second `eth_send` begins. Additionally, `arp_resolve` disables interrupts (`cli`) for its entire busy-poll duration, so the PIT ISR cannot call `virtio_net_poll` and accidentally produce an RX-triggered `eth_send` (e.g. an ICMP echo reply) while the original send is pending.

**No IP fragmentation.** Sends larger than 1480 bytes (1500 MTU - 20 IP header) fail with `EMSGSIZE`. Phase 26 socket layer enforces this limit.

**Fixed 4KB TCP send window.** `snd_wnd` advertised to peers is always 4096. Actual throughput is limited. Congestion control (Reno or CUBIC) is v2.0 work.

**No TCP options.** No MSS negotiation, no window scaling, no SACK, no timestamps. Remote peers may send options in SYN — they are parsed and ignored.

**`tcp_tick()` runs at 100Hz.** Retransmit granularity is 10ms. Minimum retransmit timeout is 100 ticks = 1 second.

**No `SO_REUSEADDR` in Phase 25.** A port in TIME_WAIT blocks rebind for 4 seconds. Phase 26 `setsockopt` implements this.

**ARP busy-poll disables interrupts.** `arp_resolve()` called from `tcp_connect()` (syscall context) disables interrupts (`cli`) for the duration of the busy-poll (up to ~100ms) to prevent the PIT ISR from concurrently advancing `rx_last_used`. This means the scheduler does not preempt during ARP resolution, and other processes starve for up to 100ms. Phase 26+ should use an async ARP queue to avoid this.
