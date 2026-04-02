# Phase 28: DHCP Userspace Daemon Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a userspace DHCP client daemon that uses the Phase 26 socket API to obtain an IP address from a DHCP server, configure the kernel network stack via `sys_netcfg`, and write resolver configuration to `/etc/resolv.conf`.

**Architecture:** A musl-linked binary (`dhcp_bin.c`) compiled like `shell_bin.c`. Uses `SOCK_DGRAM` on port 68, broadcasts DISCOVER, waits for OFFER, sends REQUEST, waits for ACK. Calls `sys_netcfg` to push IP/mask/gateway into the kernel. `init_bin.c` forks and execs it before dropping to the shell.

**Tech Stack:** C (musl), POSIX socket API (Phase 26), UDP broadcast, `sys_netcfg` (syscall 500).

---

## Constraints and Non-Negotiables

- Must use only the POSIX socket API — no kernel-internal calls. The daemon is a normal user process.
- `sys_netcfg` requires `CAP_KIND_NET_ADMIN`. **Phase 28 grants `CAP_KIND_NET_ADMIN` to ALL user processes via `proc_spawn`** — the same pattern used for `CAP_KIND_NET_SOCKET`. This is the simplest correct implementation. Fine-grained restriction (only the DHCP daemon gets NET_ADMIN) requires a capability delegation syscall or post-fork grant mechanism, both of which introduce new syscall numbers and security surface that are v2.0 work. The file layout table entry for `proc.c` reflects this: add `CAP_KIND_NET_ADMIN` to the `proc_spawn` grant list, alongside `CAP_KIND_NET_SOCKET`.
- DHCP timeout: 5 seconds total. If no ACK received, print `[DHCP] timeout: no lease obtained` to stderr and exit. `init_bin.c` continues without network.
- No lease renewal in Phase 28. The daemon exits after successful configuration. Renewal is v2.0 work.
- The daemon writes to `/etc/resolv.conf` on the ext2 filesystem if DNS servers are provided in the ACK. If the file cannot be created (ext2 not mounted), silently skip.
- Must work with QEMU SLIRP built-in DHCP server (assigns `10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`).
- `make test` unaffected — `-machine pc` has no NIC. `init_bin.c` always forks and execs `/bin/dhcp`. With no NIC, `sys_netcfg op=1` returns `-ENODEV` and the daemon exits immediately with status 1. `init_bin.c` waits up to 5 seconds (sees immediate exit) and then execs the shell. No `netdev_count()` kernel function is needed.

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `kernel/dhcp_bin.c` | Create | DHCP client implementation |
| `kernel/init_bin.c` | Modify | Fork + exec `/bin/dhcp` before shell if netdev present |
| `kernel/net/netdev.h` | No change | `netdev_count()` not needed — init always tries to exec `/bin/dhcp` |
| `kernel/proc/proc.c` | Modify | Add `CAP_KIND_NET_ADMIN` to `proc_spawn` grant list (all user processes) |
| `Makefile` | Modify | Compile `dhcp_bin.c` → `dhcp_bin.o`; add to initrd + ext2 as `/bin/dhcp` |
| `tests/test_dhcp.py` | Create | Boot q35 + virtio-net + SLIRP; verify IP assigned and ping works |
| `tests/run_tests.sh` | Modify | Add `test_dhcp.py` |

## Prerequisites from Earlier Phases

The following fixes in earlier phases are required before Phase 28 can work:

1. **Phase 25 `ip_send` must handle `255.255.255.255`** — DHCP DISCOVER/REQUEST are sent to the limited broadcast address. `ip_send` must short-circuit ARP and use `ff:ff:ff:ff:ff:ff` as the destination MAC when `dst_ip == 0xFFFFFFFF`. This is specified in the Phase 25 spec.

2. **Phase 26 `setsockopt` must support `SO_BROADCAST`** — the DHCP daemon sets this option before sending. This is specified in the Phase 26 spec.

3. **Phase 25 must permit `src_ip == 0.0.0.0`** — DHCP packets are sent before an IP address is assigned. `ip_send` must not reject sends with `src_ip == 0`. This is specified in the Phase 25 spec.

---

## sys_netcfg Read-Back Struct (op=1)

```c
/* Used by dhcp_bin.c to read MAC address before sending DISCOVER */
typedef struct {
    uint8_t    mac[6];
    uint8_t    _pad[2];
    uint32_t   ip;      /* current IP, network byte order; 0 if unconfigured */
    uint32_t   mask;
    uint32_t   gateway;
} netcfg_info_t;

/* syscall(500, 1, (uint64_t)&info, 0, 0) fills info and returns 0.
 * Returns -ENODEV if no netdev is registered. */
```

---

## DHCP Protocol (RFC 2131)

DHCP runs over UDP: client port 68, server port 67. All packets are broadcast during discovery.

### Packet Sequence

```
Client                          Server (QEMU SLIRP)
  |                                    |
  |--- DHCPDISCOVER (broadcast) ------>|
  |<-- DHCPOFFER (unicast/broadcast) --|
  |--- DHCPREQUEST (broadcast) ------->|
  |<-- DHCPACK (unicast/broadcast) ----|
```

### Packet Structure (BOOTP header, RFC 951)

```c
typedef struct __attribute__((packed)) {
    uint8_t  op;          /* 1=BOOTREQUEST, 2=BOOTREPLY */
    uint8_t  htype;       /* 1=Ethernet */
    uint8_t  hlen;        /* 6 (MAC address length) */
    uint8_t  hops;        /* 0 for client */
    uint32_t xid;         /* transaction ID (random) */
    uint16_t secs;        /* seconds since start */
    uint16_t flags;       /* 0x8000 = broadcast flag */
    uint32_t ciaddr;      /* client IP (0 for DISCOVER) */
    uint32_t yiaddr;      /* your IP (filled by server in OFFER/ACK) */
    uint32_t siaddr;      /* server IP */
    uint32_t giaddr;      /* gateway IP (0 for client) */
    uint8_t  chaddr[16];  /* client hardware address (MAC in first 6 bytes) */
    uint8_t  sname[64];   /* server hostname (zeroed) */
    uint8_t  file[128];   /* boot filename (zeroed) */
    uint32_t magic;       /* 0x63825363 — DHCP magic cookie */
    uint8_t  options[308];/* DHCP options */
} dhcp_pkt_t;

_Static_assert(sizeof(dhcp_pkt_t) == 548, "DHCP packet must be 548 bytes");
```

### DHCP Options Parsed

| Code | Meaning | Action |
|------|---------|--------|
| 1 | Subnet mask | Store, pass to `sys_netcfg` |
| 3 | Router (gateway) | Store, pass to `sys_netcfg` |
| 6 | DNS servers | Write first two to `/etc/resolv.conf` |
| 51 | Lease time | Print to serial (ignored for renewal) |
| 53 | DHCP message type | 2=OFFER, 5=ACK, 6=NAK |
| 54 | DHCP server identifier | Use in REQUEST |
| 255 | End | Stop parsing |

---

## Implementation

### Socket Setup

```c
int sock = socket(AF_INET, SOCK_DGRAM, 0);

/* Enable broadcast */
int yes = 1;
setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

/* Bind to port 68 */
struct sockaddr_in local = {
    .sin_family = AF_INET,
    .sin_port   = htons(68),
    .sin_addr   = { 0 },    /* INADDR_ANY */
};
bind(sock, (struct sockaddr *)&local, sizeof(local));

/* Set receive timeout: 5 seconds */
struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
```

### Build DHCPDISCOVER

```c
dhcp_pkt_t pkt = {0};
pkt.op    = 1;          /* BOOTREQUEST */
pkt.htype = 1;          /* Ethernet */
pkt.hlen  = 6;
pkt.xid   = xid;        /* random 32-bit value */
pkt.flags = htons(0x8000); /* request broadcast reply */
pkt.magic = htonl(0x63825363);

/* Options: message type DISCOVER (53=1), end (255) */
uint8_t *opt = pkt.options;
*opt++ = 53; *opt++ = 1; *opt++ = 1;  /* DHCPDISCOVER */
*opt++ = 61; *opt++ = 7; *opt++ = 1;  /* client identifier: type=Ethernet */
memcpy(opt, mac, 6); opt += 6;
*opt++ = 255;  /* end */

memcpy(pkt.chaddr, mac, 6);
```

### Send + Receive Loop

```c
struct sockaddr_in bcast = {
    .sin_family = AF_INET,
    .sin_port   = htons(67),
    .sin_addr   = { 0xFFFFFFFF }, /* 255.255.255.255 */
};

sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&bcast, sizeof(bcast));

/* Wait for OFFER */
dhcp_pkt_t reply;
struct sockaddr_in from;
socklen_t fromlen = sizeof(from);
ssize_t n = recvfrom(sock, &reply, sizeof(reply), 0,
                     (struct sockaddr *)&from, &fromlen);
if (n < 0) { /* timeout */ exit(1); }

/* Verify xid matches, parse options to find DHCPOFFER (type 53 == 2) */
/* Build DHCPREQUEST (broadcast, same xid, ciaddr=0): */
uint8_t *opt = request_pkt.options;
*opt++ = 53; *opt++ = 1; *opt++ = 3;              /* DHCPREQUEST */
*opt++ = 54; *opt++ = 4;                            /* server identifier */
memcpy(opt, &server_id, 4); opt += 4;              /* from OFFER option 54 */
*opt++ = 50; *opt++ = 4;                            /* requested IP */
memcpy(opt, &offered_ip, 4); opt += 4;             /* yiaddr from OFFER */
*opt++ = 255;                                        /* end */
/* Send REQUEST to broadcast, wait for DHCPACK (type 53 == 5) or NAK (type 53 == 6) */
/* Parse yiaddr, subnet mask (option 1), gateway (option 3), DNS (option 6) */
```

### Configure Kernel

```c
/* Read MAC address before building DISCOVER.
 * sys_netcfg op=1: kernel fills netcfg_info_t via copy_to_user into &info.
 * The struct is on the DHCP daemon's stack — a valid user-space address. */
netcfg_info_t info;
syscall(500, 1, (uint64_t)&info, 0, 0);
memcpy(pkt.chaddr, info.mac, 6);

/* After ACK: set IP config */
/* syscall 500: sys_netcfg(op=0, ip, mask, gw) */
syscall(500, 0, yiaddr, subnet_mask, gateway);

/* Write /etc/resolv.conf */
int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC);
if (fd >= 0) {
    dprintf(fd, "nameserver %u.%u.%u.%u\n", dns1 bytes...);
    close(fd);
}

/* Print confirmation */
fprintf(stderr, "[DHCP] lease: %u.%u.%u.%u/... gw %u.%u.%u.%u\n", ...);
```

### `init_bin.c` Integration

`init_bin.c` always forks and execs `/bin/dhcp`. The daemon itself handles the no-NIC case: `sys_netcfg op=1` returns `-ENODEV` when no netdev is registered, the daemon prints nothing and exits with status 1. `init_bin.c` detects the fast exit and continues to the shell.

```c
pid_t dhcp_pid = fork();
if (dhcp_pid == 0) {
    execve("/bin/dhcp", argv_dhcp, envp);
    _exit(1); /* execve failed — no dhcp binary or no network */
}
/* Wait for DHCP daemon to exit (max 5 seconds), then exec the shell.
 * nanosleep yields the CPU so the daemon can receive network packets. */
for (int i = 0; i < 50; i++) {
    int status;
    if (waitpid(dhcp_pid, &status, WNOHANG) > 0) break; /* daemon exited */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
    nanosleep(&ts, NULL);
}
/* exec shell regardless of DHCP outcome */
```

---

## MAC Address Access

The DHCP daemon needs the hardware MAC address. Two options:
1. **`sys_netcfg` op 1: get config** — returns current IP + MAC. The daemon calls this before DISCOVER.
2. **`/sys/net/eth0/mac` virtual file** — a new VFS pseudo-file. Cleaner but more work.

Phase 28 uses option 1: extend `sys_netcfg` with `op=1` (read) returning MAC + current IP in a struct.

---

## Testing

### `tests/test_dhcp.py`

Boots with `-machine q35 -device virtio-net-pci,disable-legacy=on -netdev user,id=n0`.

QEMU SLIRP has a built-in DHCP server: assigns `10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`.

```python
# Wait for DHCP confirmation in serial output
assert "[DHCP] lease: 10.0.2.15" in output

# Send ping via shell
send_keys("ping 10.0.2.2\n")
assert "echo reply" in read_until("#", timeout=10)
```

QEMU user networking responds to ICMP ping natively — no host configuration needed.

---

## Forward-Looking Constraints

**No lease renewal.** The daemon exits after ACK. If the lease expires (QEMU SLIRP leases are typically 24h), the kernel retains the stale IP. A renewal daemon would need to sleep and re-negotiate before expiry. v2.0 work.

**`sys_netcfg` is Aegis-specific.** Standard tools (`dhclient`, `NetworkManager`) use `ioctl(SIOCSIFADDR)` / `netlink`. A future phase may implement the `rtnetlink` interface so standard userspace network management tools work.

**MAC address access is awkward.** `sys_netcfg op=1` is a workaround. A proper `ioctl(SIOCGIFHWADDR)` on a socket would be the POSIX approach.

**No DHCPv6.** IPv6 is not in scope for Phases 24–28.

**DNS not used.** `/etc/resolv.conf` is written but nothing in the kernel reads it. A future `sys_getaddrinfo` or userspace resolver library would use it.

**Single network interface assumed.** `dhcp_bin.c` hardcodes `"eth0"`. Multi-interface DHCP requires iterating registered netdevs — v2.0 work.
