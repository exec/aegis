# Phase 44: IPC — Unix Domain Sockets, fd Passing, Shared Memory

**Date:** 2026-03-30
**Status:** Approved
**Depends on:** Phase 42b (current baseline), socket API (Phase 26), mmap (Phase 30)

## Goal

Add inter-process communication primitives that unlock external GUI applications (Glyph clients talking to Lumen) and capability helper daemons (Phase 45). Until Phase 44, all GUI apps must be compiled into Lumen.

Three subsystems:
1. **AF_UNIX SOCK_STREAM** — local byte-stream sockets with pathname binding
2. **sendmsg/recvmsg + SCM_RIGHTS** — fd passing between processes
3. **memfd_create + MAP_SHARED** — shared anonymous memory regions

## Non-Goals

- SOCK_DGRAM unix sockets (no consumer)
- Abstract namespace (`\0`-prefixed paths)
- SCM_CREDENTIALS ancillary data (SO_PEERCRED covers authentication)
- File-backed MAP_SHARED (requires page cache)
- shm_open/shm_unlink (memfd_create + fd passing is equivalent and simpler)

---

## Component 1: AF_UNIX SOCK_STREAM

### Data Structures

New file: `kernel/net/unix_socket.c` + `unix_socket.h`

```c
#define UNIX_SOCK_MAX    32
#define UNIX_PATH_MAX    108    // POSIX standard
#define UNIX_BUF_SIZE    4056   // one kva page with metadata, matches pipe model

typedef struct unix_sock {
    uint8_t  in_use;
    uint8_t  state;            // UNIX_FREE/BOUND/LISTENING/CONNECTED/CLOSED
    uint8_t  nonblocking;
    uint8_t  _pad;
    char     path[UNIX_PATH_MAX]; // bound pathname (empty = unbound)

    // Ring buffer — one per direction, kva-allocated
    uint8_t *tx_ring;          // this socket writes here, peer reads
    uint16_t tx_head;
    uint16_t tx_tail;

    // Peer link
    uint32_t peer_id;          // index of connected peer (0xFFFFFFFF = none)

    // Accept queue (listening sockets)
    uint32_t accept_queue[8];
    uint8_t  accept_head, accept_tail;

    // Pending connections (connecting sockets waiting for accept)
    uint32_t pending_queue[8];
    uint8_t  pending_head, pending_tail;

    // Blocking
    aegis_task_t *waiter_task;

    // Peer credentials (captured at connect time)
    uint32_t peer_pid;
    uint32_t peer_uid;
    uint32_t peer_gid;

    // fd passing staging area
    passed_fd_t passed_fds[16];
    uint8_t     passed_fd_count;

    // Refcount for dup/fork
    uint32_t refcount;
} unix_sock_t;

typedef struct {
    const vfs_ops_t *ops;
    void            *priv;
    uint32_t         flags;
} passed_fd_t;
```

### Name Table (Hybrid Approach)

Kernel-internal name table for socket rendezvous, plus ramfs stubs for filesystem visibility.

```c
#define UNIX_NAME_MAX 32

typedef struct {
    char     path[UNIX_PATH_MAX];
    uint32_t sock_id;
    uint8_t  in_use;
} unix_name_entry_t;

static unix_name_entry_t s_unix_names[UNIX_NAME_MAX];
```

- `bind()` adds a name entry. If the path falls under a ramfs mount (`/run/`, `/tmp/`), also creates an S_IFSOCK stub for filesystem visibility. Paths under other filesystems (ext2, initrd) register in the name table but get no filesystem stub.
- `close()` on last refcount removes name entry + ramfs stub (if one was created)
- `connect()` looks up path in name table to find the listening socket
- `stat()` on the ramfs stub returns `S_IFSOCK | 0666`

### Connection Flow

1. **Server:** `socket(AF_UNIX, SOCK_STREAM, 0)` -> `bind("/run/lumen.sock")` -> `listen(backlog)` -> `accept()`
2. **Client:** `socket(AF_UNIX, SOCK_STREAM, 0)` -> `connect("/run/lumen.sock")`
3. `connect()` finds server in name table, allocates ONE new unix_sock_t entry (server-side of the connected pair), cross-links it with the client's existing entry via `peer_id`, allocates two kva pages for ring buffers (one per direction), captures client credentials into the server-side entry, enqueues server-side sock_id into the listening socket's accept queue, wakes server
4. `accept()` dequeues from accept queue, creates a new fd pointing to the server-side unix_sock_t, returns it. The listening socket itself is not consumed.
5. `send()`/`recv()` (or `write()`/`read()` via VFS ops): write into own tx_ring, peer reads from it

### Ring Buffers

Each connected pair gets TWO kva pages (one ring per direction):
- Socket A's tx_ring = Socket B's read source
- Socket B's tx_ring = Socket A's read source

This matches the pipe model exactly. Writer blocks when ring is full (woken by reader draining). Reader blocks when ring is empty (woken by writer filling).

### VFS Integration

Unix sockets get their own `vfs_ops_t`:
- `read` -> read from peer's tx_ring (blocking)
- `write` -> write to own tx_ring (blocking)
- `close` -> decrement refcount, free on zero, wake blocked peer with EIO/EPIPE
- `dup` -> increment refcount
- `stat` -> return S_IFSOCK | 0666

### Socket Syscall Dispatch

`sys_socket()` gains an `AF_UNIX` branch:
- `AF_UNIX (1)` -> `unix_sock_alloc()` + `sock_open_fd()` with unix VFS ops
- `AF_INET (2)` -> existing path

`sys_bind/listen/accept/connect/sendto/recvfrom` check whether the fd's VFS ops are unix or inet and dispatch accordingly. The cleanest approach: check `ops == &s_unix_sock_ops` in each syscall.

---

## Component 2: sendmsg/recvmsg + SCM_RIGHTS

### Syscall Interface

Syscalls 46 (sendmsg) and 47 (recvmsg) — stubs already wired.

```c
// Linux ABI structs (parsed from userspace)
struct msghdr {
    void         *msg_name;       // ignored for connected SOCK_STREAM
    uint32_t      msg_namelen;
    struct iovec *msg_iov;        // scatter/gather data
    uint64_t      msg_iovlen;
    void         *msg_control;    // ancillary data
    uint64_t      msg_controllen;
    int           msg_flags;
};

struct cmsghdr {
    uint64_t cmsg_len;     // total length including header
    int      cmsg_level;   // SOL_SOCKET (1)
    int      cmsg_type;    // SCM_RIGHTS (1)
    // payload: int fds[]
};

#define SOL_SOCKET   1
#define SCM_RIGHTS   1
```

### sendmsg Flow

1. `copy_from_user` the `struct msghdr`
2. Copy iov data into own tx_ring (same as regular write/send)
3. If `msg_controllen > 0`, parse cmsghdr:
   - Validate `cmsg_level == SOL_SOCKET`, `cmsg_type == SCM_RIGHTS`
   - Extract sender's fd numbers from cmsg payload
   - For each fd: validate exists in sender's fd_table, call `ops->dup(priv)`, stage as `passed_fd_t` in peer's `passed_fds[]` array
   - If staging full (16 slots), return `-ENOBUFS`
4. Wake peer

### recvmsg Flow

1. Read data from peer's tx_ring into iov (same as regular read/recv)
2. If `passed_fd_count > 0` on this socket AND user provided `msg_control` buffer:
   - For each staged fd: find free slot in receiver's fd_table, install ops/priv/flags
   - Build cmsghdr with `SCM_RIGHTS` and the new fd numbers
   - `copy_to_user` the ancillary data
   - Clear staging area
3. If `passed_fd_count > 0` but user provided no `msg_control`, fds are silently dropped (closed). This matches Linux behavior.

### Scope

- Only AF_UNIX SOCK_STREAM (not INET, not DGRAM)
- Only SCM_RIGHTS (not SCM_CREDENTIALS)
- sendmsg/recvmsg on AF_INET fds fall through to sendto/recvfrom (data only, cmsg ignored)

---

## Component 3: memfd_create + MAP_SHARED

### memfd_create (syscall 319)

New file: `kernel/fs/memfd.c` + `memfd.h`

```c
#define MEMFD_MAX        16      // system-wide limit
#define MEMFD_PAGES_MAX  2048    // 8MB max (one 1080p BGRA framebuffer)

typedef struct {
    uint8_t   in_use;
    uint32_t  refcount;
    char      name[32];
    uint64_t  phys_pages[MEMFD_PAGES_MAX];
    uint32_t  page_count;       // allocated pages
    uint64_t  size;             // logical size (set by ftruncate)
} memfd_t;
```

**Syscall:** `memfd_create(const char *name, unsigned int flags)`
- Gated by `CAP_KIND_IPC`
- Allocates a `memfd_t` slot, returns an fd with memfd VFS ops
- Initial size = 0, page_count = 0
- `flags` ignored for v1

**VFS ops:**
- `read` -> copy from physical pages at offset (via window map)
- `write` -> copy to physical pages, grow if needed
- `close` -> decrement refcount, free all physical pages when zero
- `dup` -> increment refcount
- `stat` -> return size, S_IFREG | 0600

### ftruncate (syscall 77)

Currently a stub. Implement for memfd fds only:
- If growing: allocate physical pages via pmm_alloc_page, zero them
- If shrinking: free excess pages via pmm_free_page
- Update `page_count` and `size`
- Non-memfd fds: return `-EINVAL` (ext2 ftruncate is future work)

### MAP_SHARED in sys_mmap

Changes to `kernel/syscall/sys_memory.c`:

1. Remove: `if (flags & MAP_SHARED) return -EINVAL;`
2. Add MAP_SHARED path:
   - Verify fd is a memfd (check VFS ops)
   - Verify memfd has been ftruncated to sufficient size
   - Allocate VA range from mmap freelist (or bump allocator)
   - Map the memfd's physical pages into the calling process's page table via `vmm_map_user_page(pml4, va, phys, flags)`
   - Create VMA with `type = VMA_SHARED (8)`
3. On `sys_munmap` of a `VMA_SHARED` region: unmap PTEs but do NOT free physical pages (memfd owns them)

### VMA Type

```c
#define VMA_SHARED 8   // MAP_SHARED mapping — physical pages owned by memfd
```

---

## Component 4: Capability + Integration

### CAP_KIND_IPC = 15

- Gates: `socket(AF_UNIX)` and `memfd_create()`
- Granted in execve baseline (all exec'd binaries get it)
- Vigil exec_caps: add/remove via `IPC` in caps file
- stsh sandbox: `sandbox -IPC ./app` strips local IPC

### SO_PEERCRED

```c
struct ucred {
    int pid;
    int uid;
    int gid;
};
```

- `getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len)` on AF_UNIX connected sockets
- Credentials captured at connect() time from the connecting process
- Future expansion: capability bitmap (Phase 45)

### Existing Syscalls Modified

| Syscall | Change |
|---------|--------|
| socket (41) | AF_UNIX branch |
| bind (49) | AF_UNIX name registration + ramfs stub |
| listen (50) | AF_UNIX transition to LISTENING |
| accept (43) | AF_UNIX accept queue dequeue |
| connect (42) | AF_UNIX name lookup + connection setup |
| sendmsg (46) | Full implementation (data + SCM_RIGHTS) |
| recvmsg (47) | Full implementation (data + SCM_RIGHTS) |
| getsockopt (55) | SO_PEERCRED for AF_UNIX |
| mmap (9) | MAP_SHARED for memfd fds |
| munmap (11) | VMA_SHARED: don't free phys pages |

### New Syscalls

| Syscall | Number | Gate |
|---------|--------|------|
| memfd_create | 319 | CAP_KIND_IPC |
| ftruncate | 77 | none (only works on memfd fds) |

---

## Test Plan

New test binary: `user/ipc_test/main.c` (statically linked)

**Test cases:**
1. **Unix socket echo** — server bind+listen+accept, client connect, send "hello", recv "hello" back
2. **SO_PEERCRED** — server accepts, getsockopt SO_PEERCRED, verify pid > 0 and uid/gid match
3. **fd passing** — sender creates memfd, writes 0xDEADBEEF at offset 0, passes fd via sendmsg SCM_RIGHTS, receiver recvmsg, reads memfd, verifies 0xDEADBEEF
4. **MAP_SHARED** — parent creates memfd, ftruncate to 4096, mmap MAP_SHARED, spawns child, child mmap same memfd (fd passed or inherited), parent writes value, child reads same value
5. **Error cases** — connect to nonexistent path returns ECONNREFUSED, socket(AF_UNIX) without CAP_KIND_IPC returns ENOCAP

Integrated into `test_integrated.py` as `ipc: /bin/ipc_test`.

**Boot oracle:** No new `[SUBSYSTEM] OK` lines. IPC is invisible kernel infrastructure.

---

## Forward Constraints

1. **UNIX_SOCK_MAX = 32.** Each connected pair uses 2 slots + 2 kva pages. 16 concurrent connections max. Sufficient for Phase 44-45; expand if needed.

2. **MEMFD_PAGES_MAX = 2048 (8MB).** One 1080p BGRA framebuffer fits exactly. 4K resolution (3840x2160x4 = ~32MB) would need 8192 pages — expand later.

3. **memfd_t BSS: ~256KB.** 16 slots x 16KB each (dominated by phys_pages array). Acceptable; kernel BSS limit is ~6MB.

4. **No SOCK_DGRAM AF_UNIX.** Only SOCK_STREAM. Add if a consumer appears.

5. **No abstract namespace.** Paths must start with `/`. Add `\0`-prefixed abstract namespace if needed for containerization.

6. **ftruncate only for memfd.** ext2 ftruncate deferred.

7. **SO_PEERCRED is read-only standard ucred.** Phase 45 may extend with capability bitmap for efficiency.

8. **No MSG_PEEK, MSG_DONTWAIT, MSG_WAITALL.** Basic sendmsg/recvmsg only. Add flags as needed.

9. **Passed fds silently dropped if receiver provides no msg_control buffer.** Matches Linux behavior but can leak resources if receiver is buggy.

10. **MAP_SHARED only for memfd fds.** Not for ext2 files or pipes. File-backed shared mapping requires a page cache.
