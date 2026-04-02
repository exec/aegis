# Phase 26: Full POSIX Socket API + epoll Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Expose the Phase 25 protocol stack to user processes via a complete POSIX socket API, including blocking operations, non-blocking mode, `poll`, `select`, and `epoll`.

**Architecture:** Sockets are VFS file descriptors — `sys_socket()` allocates a `vfs_file_t` slot in `proc->fds[]` backed by a socket-specific `vfs_ops_t`. A kernel `sock_t` table (64 entries) holds per-socket state. `sched_block`/`sched_wake` provide blocking semantics. epoll uses a separate event table (8 instances, 64 watches each).

**Tech Stack:** C, existing VFS fd machinery, existing `sched_block`/`sched_wake`, musl-compatible ABI.

---

## Constraints and Non-Negotiables

- Only `AF_INET` (IPv4) in v1. `AF_UNIX` and `AF_INET6` return `EAFNOSUPPORT`.
- Only `SOCK_STREAM` (TCP) and `SOCK_DGRAM` (UDP) in v1. `SOCK_RAW` returns `EPROTONOSUPPORT` (reserved for future ICMP raw sockets).
- `sock_t` table: 64 entries. `epoll_fd_t` table: 8 instances, 64 watches per instance.
- All socket operations go through `copy_from_user`/`copy_to_user` for pointer arguments (SMAP safety).
- `sockaddr_in` layout must match musl's `struct sockaddr_in` exactly (verified with `_Static_assert`).
- Capability gate: `sys_socket()` requires a new `CAP_KIND_NET_SOCKET` capability. `proc_spawn` grants it to all user processes in Phase 26 (same pattern as `CAP_KIND_VFS_OPEN`).
- `sys_netcfg` (for DHCP daemon) is capability-gated with `CAP_KIND_NET_ADMIN`. Granted to ALL user processes via `proc_spawn` (Phase 28 simplification — fine-grained restriction to only the DHCP daemon requires a capability delegation mechanism that is v2.0 work).
- `make test` unaffected — socket syscalls added to dispatch table with no boot-time side effects.

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `kernel/net/socket.h` | Create | `sock_t` struct, socket table API |
| `kernel/net/socket.c` | Create | Socket table, `sock_alloc`, `sock_get`, blocking/wake |
| `kernel/net/epoll.h` + `epoll.c` | Create | epoll instance table, `epoll_notify()` |
| `kernel/syscall/sys_socket.c` | Create | All socket syscall implementations |
| `kernel/httpd_bin.c` | Create | Minimal HTTP server binary; `socket/bind/listen/accept/recv/send/close`; ships as `/bin/httpd` in ext2 |
| `kernel/syscall/syscall.c` | Modify | Dispatch new syscall numbers |
| `kernel/cap/src/lib.rs` | Modify | Add `CAP_KIND_NET_SOCKET`, `CAP_KIND_NET_ADMIN` |
| `kernel/proc/proc.c` | Modify | Grant `CAP_KIND_NET_SOCKET` and `CAP_KIND_NET_ADMIN` in `proc_spawn` (Phase 28 DHCP needs NET_ADMIN) |
| `kernel/net/tcp.c` | Modify | Call `epoll_notify()` + `sched_wake()` on data/connect events |
| `kernel/net/udp.c` | Modify | Call `epoll_notify()` + `sched_wake()` on received datagrams |
| `tests/test_socket.py` | Create | Boot q35 + virtio-net + SLIRP; DHCP; HTTP server; verify response |
| `tests/run_tests.sh` | Modify | Add `test_socket.py` |

---

## Syscall Numbers

Follow Linux x86-64 ABI (musl uses these):

| Syscall | Number | Notes |
|---------|--------|-------|
| `socket` | 41 | |
| `connect` | 42 | |
| `accept` | 43 | |
| `sendto` | 44 | covers `send` (NULL addr) |
| `recvfrom` | 45 | covers `recv` (NULL addr) |
| `sendmsg` | 46 | |
| `recvmsg` | 47 | |
| `shutdown` | 48 | |
| `bind` | 49 | |
| `listen` | 50 | |
| `getsockname` | 51 | |
| `getpeername` | 52 | |
| `socketpair` | 53 | in-kernel ring buffer pair (AF_UNIX or AF_INET; no IP stack) |
| `setsockopt` | 54 | SO_REUSEADDR, SO_BROADCAST, SO_RCVTIMEO, SO_SNDTIMEO, TCP_NODELAY |
| `getsockopt` | 55 | mirrors setsockopt options |
| `select` | 23 | already reserved; implement now |
| `poll` | 7 | already reserved; implement now |
| `epoll_create1` | 291 | |
| `epoll_ctl` | 233 | |
| `epoll_wait` | 232 | |
| `sys_netcfg` | 500 | Aegis-specific; sets IP/mask/gw; CAP_KIND_NET_ADMIN |

`fcntl` (72) already exists — extend to handle `F_GETFL`/`F_SETFL` with `O_NONBLOCK` for socket fds.

---

## Socket Object (`sock_t`)

```c
#define SOCK_TABLE_SIZE 64

typedef enum {
    SOCK_FREE, SOCK_CREATED, SOCK_BOUND, SOCK_LISTENING,
    SOCK_CONNECTING, SOCK_CONNECTED, SOCK_CLOSED
} sock_state_t;

typedef struct {
    sock_state_t state;
    uint8_t      type;         /* SOCK_STREAM or SOCK_DGRAM */
    uint8_t      nonblocking;  /* set by fcntl O_NONBLOCK */
    ip4_addr_t   local_ip;
    uint16_t     local_port;
    ip4_addr_t   remote_ip;
    uint16_t     remote_port;
    uint32_t     tcp_conn_id;  /* index into tcp_conn table; UINT32_MAX if none */
    /* accept queue for LISTEN sockets: ring of completed conn ids */
    uint32_t     accept_queue[8];
    uint8_t      accept_head, accept_tail;
    /* blocking waiter: one task waiting on recv/accept/connect.
     * Store aegis_task_t* directly — matches sched_wake() signature and
     * the established pipe.c pattern (reader_waiting/writer_waiting). */
    aegis_task_t *waiter_task; /* NULL = none */
    uint32_t     epoll_id;     /* epoll instance watching this socket; UINT32_MAX = none */
    uint64_t     epoll_events; /* EPOLLIN | EPOLLOUT mask */
    uint8_t      reuseaddr;
} sock_t;

static sock_t s_socks[SOCK_TABLE_SIZE];
```

---

## Blocking Model

When `recv()` / `accept()` / `connect()` would block:
1. Set `sock->waiter_task = current` (the `aegis_task_t *` for the calling process)
2. Call `sched_block(current)` — process removed from run queue
3. When TCP layer delivers data/connection: call `sock_wake(sock_id)` which calls `sched_wake(sock->waiter_task)`
4. Set `sock->waiter_task = NULL`
5. Process re-enters syscall path, checks condition again (spurious-wake safe)

For `O_NONBLOCK`: skip steps 1–4, return `-EAGAIN` immediately.

For `SO_RCVTIMEO` / `SO_SNDTIMEO`: use a PIT-tick deadline. If `arch_get_ticks() >= deadline` before wakeup, return `-ETIMEDOUT`.

---

## epoll Design

```c
#define EPOLL_MAX_INSTANCES 8
#define EPOLL_MAX_WATCHES   64

typedef struct {
    uint32_t fd;
    uint64_t events;   /* EPOLLIN, EPOLLOUT, EPOLLERR, EPOLLHUP, EPOLLET */
    uint64_t data;     /* user data (epoll_data_t union) */
} epoll_watch_t;

typedef struct {
    uint8_t          in_use;
    epoll_watch_t    watches[EPOLL_MAX_WATCHES];
    uint8_t          nwatches;
    aegis_task_t    *waiter_task; /* NULL = none; same type as sched_wake() argument */
    /* ready list: indices into watches[] that are ready */
    uint8_t          ready[EPOLL_MAX_WATCHES];
    uint8_t          nready;
} epoll_fd_t;

static epoll_fd_t s_epoll[EPOLL_MAX_INSTANCES];
```

`epoll_notify(sock_id, events)` — called by TCP/UDP layers when data arrives or connection completes. Reads `sock_t.epoll_id` to find the single epoll instance watching this socket (UINT32_MAX = none), marks it ready, wakes any blocked `epoll_wait`. Only one epoll instance can watch a given socket at a time (enforced by `epoll_ctl` — `EPOLL_CTL_ADD` on a socket already watched by another epoll instance returns `-EBUSY`).

`epoll_wait` with `timeout == 0`: non-blocking check. `timeout == -1`: block indefinitely. `timeout > 0`: block with PIT-tick deadline.

Edge-triggered (`EPOLLET`) is supported: ready flag is cleared on each `epoll_wait` return, only re-set when new data arrives.

---

## Key Syscall Implementations

### `sys_socket(domain, type, proto)`

1. Verify `domain == AF_INET`, `type == SOCK_STREAM || SOCK_DGRAM`
2. Check `CAP_KIND_NET_SOCKET` capability
3. Allocate `sock_t` slot
4. Allocate `vfs_file_t` fd slot with socket `vfs_ops_t`
5. Return fd

### `sys_bind(fd, addr, addrlen)`

1. `copy_from_user` → `struct sockaddr_in`
2. Validate port not already bound (scan UDP/TCP binding tables)
3. Store `local_ip`, `local_port` in `sock_t`
4. Register in TCP/UDP layer binding table

### `sys_listen(fd, backlog)`

1. Set `sock->state = SOCK_LISTENING`
2. Register in TCP layer as LISTEN entry
3. `backlog` clamped to 8 (accept queue size)

### `sys_accept(fd, addr, addrlen)`

1. If accept queue non-empty: dequeue completed `tcp_conn_id`, allocate a new `sock_t` slot (distinct from the listening socket) with `state=SOCK_CONNECTED`, `tcp_conn_id` set to the dequeued value, `local_ip`/`local_port`/`remote_ip`/`remote_port` copied from `tcp_conn_t`, allocate a new fd slot pointing to this `sock_t`, fill `addr` with `remote_ip`/`remote_port`, return the new fd.
2. If empty and blocking: set `sock->waiter_task = current`, `sched_block()`, retry on wake
3. If empty and `O_NONBLOCK`: return `-EAGAIN`

### `sys_netcfg(op, arg1, arg2, arg3)`

```c
/* op 0: set IP config */
/* arg1 = ip (network byte order), arg2 = mask, arg3 = gateway */
/* Requires CAP_KIND_NET_ADMIN. Calls net_set_config(ip, mask, gw).
 * Prints "[NET] configured: %u.%u.%u.%u/%u gw %u.%u.%u.%u". */

/* op 1: read current config + MAC address */
/* arg1 = user pointer to netcfg_info_t (see below) */
/* Requires CAP_KIND_NET_ADMIN. Fills struct via copy_to_user and returns 0.
 * Returns -ENODEV if no netdev is registered. */
typedef struct {
    uint8_t    mac[6];
    uint8_t    _pad[2];
    uint32_t   ip;      /* current IP, network byte order; 0 if unconfigured */
    uint32_t   mask;
    uint32_t   gateway;
} netcfg_info_t;
/* Implementation: call netdev_get("eth0"), copy mac from dev->mac,
 * call net_get_config(&ip, &mask, &gw), then copy_to_user(arg1, &info, sizeof(info)). */
```

### `socketpair(domain, type, proto, sv[2])`

Creates two connected SOCK_STREAM sockets backed by a **shared in-kernel ring buffer pair** — no IP stack involvement, no ARP, no loopback routing. Each socket has a 4KB send ring that is the other socket's receive ring (cross-linked). Reads and writes go directly to/from the ring buffers via `copy_from_user`/`copy_to_user`. This is the correct POSIX behavior (socketpair traffic never hits the wire). Allocates two `sock_t` entries and two fd slots. `domain` must be `AF_UNIX` or `AF_INET`; both are handled identically (in-kernel buffer only). Used for IPC and testing without network hardware.

---

## `sockaddr_in` Layout Verification

```c
#include <stdint.h>
typedef struct {
    uint16_t sin_family;   /* AF_INET = 2 */
    uint16_t sin_port;     /* network byte order */
    uint32_t sin_addr;     /* network byte order */
    uint8_t  sin_zero[8];  /* padding */
} k_sockaddr_in_t;

_Static_assert(sizeof(k_sockaddr_in_t) == 16,
    "k_sockaddr_in_t must be 16 bytes (matches musl struct sockaddr_in)");
```

---

## Testing

### `tests/test_socket.py`

Uses the Phase 25 static IP (`10.0.2.15`) — the `dhcp` binary is Phase 28 and is not available yet.

1. Boot with `-machine q35 -device virtio-net-pci,disable-legacy=on -netdev user,id=n0,hostfwd=tcp::8080-:80`
2. Wait for shell prompt (IP is already `10.0.2.15` from Phase 25 static config)
3. Type `httpd &` (minimal HTTP server binary, binds port 80, responds to `GET /`)
4. From Python test: connect via stdlib `http.client.HTTPConnection("localhost", 8080)` → verify 200 response and `"Hello from Aegis"` body
5. Verify `[NET] OK: virtio-net eth0` in serial output

`httpd` binary: shipped in ext2 `/bin/httpd`. Minimal — `socket`/`bind`/`listen`/`accept`/`recv`/`send`/`close` loop. Responds with `HTTP/1.0 200 OK\r\n\r\nHello from Aegis\r\n`.

---

## Forward-Looking Constraints

**`AF_UNIX` sockets deferred.** `socketpair` is implemented via a shared in-kernel ring buffer pair — no IP stack, no loopback routing, no ARP. True Unix domain sockets with path-based addressing require a separate namespace and are v2.0 work.

**`epoll` max 8 instances, 64 watches.** Sufficient for a simple server. nginx-style event loops with thousands of connections require larger tables or dynamic allocation.

**`select`/`poll` linear scan.** `poll(fds, nfds, timeout)` scans all `nfds` entries. For nfds > 100, performance degrades. `epoll` is the recommended path for high-connection servers.

**Single waiter per socket.** `sock_t.waiter_task` holds one blocked `aegis_task_t *`. Multiple threads blocking on the same socket fd are not supported (only one will be woken). Multi-threaded `accept` requires a wait queue list — v2.0 work.

**`SO_REUSEPORT` not implemented.** `SO_REUSEADDR` is implemented (allows rebind while in TIME_WAIT). `SO_REUSEPORT` (multiple sockets on same port) is deferred.

**No `sendfile`.** File-to-socket zero-copy requires VFS integration with the network stack. Deferred.

**`sys_netcfg` is Aegis-specific.** musl programs call `ioctl(SIOCSIFADDR)` to configure network interfaces. A future phase may implement the `ioctl` socket interface so standard tools (`ifconfig`, `ip`) work without modification.
