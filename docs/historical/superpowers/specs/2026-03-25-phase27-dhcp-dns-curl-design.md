# Phase 27: DHCP + DNS + Writable /etc + /root + BearSSL + curl

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the hardcoded static IP in `net_init()` with a fully functional userspace DHCP client daemon (lease acquisition + renewal + exponential backoff), add DNS via `/etc/resolv.conf`, make `/etc` and `/root` writable (ramfs-backed), and add BearSSL + curl for HTTPS from the shell.

**Architecture:** Four sequential deliverables — (1) ramfs refactored to multi-instance + `/etc` and `/root` wired in, (2) `net_init()` stripped of static IP and ICMP self-test, (3) `user/dhcp/main.c` musl-static DHCP/DNS daemon as a vigil service, (4) BearSSL 0.6 + curl 8.13.0 compiled against musl, placed on the ext2 disk image.

**Tech Stack:** C (musl-static userspace), DHCP RFC 2131, multi-instance ramfs VFS extension, BearSSL 0.6, curl 8.13.0, ext2.

**Prerequisites:** Phase 26 socket API complete ✅ (`SO_RCVTIMEO`/`SO_SNDTIMEO` implemented in `sys_setsockopt`, confirmed in `kernel/syscall/sys_socket.c`).

---

## Part 1 — Multi-Instance Ramfs + Writable /etc and /root

### Problem

`/etc` and `/root` are fake directory stubs. Files under `/etc/` are served read-only from initrd; there is no write path on a live ISO boot (no NVMe disk). `/root` is stat-able but has no backing store. `touch /etc/foo` and `mkdir /root/bar` both fail.

The current ramfs is a single-instance module with file-static state (`s_ramfs[16]`). All public functions (`ramfs_open`, `ramfs_stat_path`, `ramfs_readdir_fn`) operate on one global array. Adding `/etc` and `/root` requires either a full instance refactor or a second module file.

### Fix: Refactor ramfs to multi-instance

Introduce a `ramfs_t` struct and thread it through all APIs:

```c
/* kernel/fs/ramfs.h */
#define RAMFS_MAX_FILES   32
#define RAMFS_MAX_NAMELEN 64
#define RAMFS_MAX_SIZE    4096

typedef struct {
    char     name[RAMFS_MAX_NAMELEN];
    uint8_t *data;
    uint32_t size;
    uint8_t  in_use;
} ramfs_file_t;

typedef struct {
    ramfs_file_t files[RAMFS_MAX_FILES];
} ramfs_t;

void ramfs_init    (ramfs_t *inst);
int  ramfs_open    (ramfs_t *inst, const char *name, int flags, vfs_file_t *out);
int  ramfs_stat    (ramfs_t *inst, const char *name, k_stat_t *out);
int  ramfs_opendir (ramfs_t *inst, vfs_file_t *out);  /* for getdents64 */
```

All internal helpers (`rfs_find`, `rfs_alloc`, `rfs_streq`, etc.) take `ramfs_t *inst` and operate on `inst->files[]` instead of the global array.

**Three instances declared in `vfs.c`:**
```c
static ramfs_t s_run_ramfs;   /* /run/ — existing, unchanged semantics */
static ramfs_t s_etc_ramfs;   /* /etc/ — new, populated from initrd at boot */
static ramfs_t s_root_ramfs;  /* /root/ — new, empty */
```

**`vfs_init()` additions:**
1. `ramfs_init(&s_run_ramfs)` — replaces old `ramfs_init()`
2. `ramfs_init(&s_etc_ramfs)` — new
3. `ramfs_init(&s_root_ramfs)` — new
4. Walk all initrd entries; for each whose path starts with `/etc/`:
   - Read content from initrd into a kernel buffer
   - Call `ramfs_populate(&s_etc_ramfs, name_without_etc_prefix, kbuf, len)` (see below)
   - Prefix rule: `/etc/motd` → name `"motd"`, `/etc/vigil/services/getty/run` → name `"vigil/services/getty/run"`
5. Create `/etc/resolv.conf` as empty slot: `ramfs_populate(&s_etc_ramfs, "resolv.conf", NULL, 0)`

**`ramfs_populate()` — kernel-side write helper (bypasses user_ptr_valid):**

`ramfs_write_fn` calls `user_ptr_valid(buf, len)` which rejects kernel pointers (SMAP guard). Kernel-side population at boot must bypass this. Add a dedicated helper that writes directly from a kernel buffer:

```c
/* kernel/fs/ramfs.h */
int ramfs_populate(ramfs_t *inst, const char *name, const uint8_t *kbuf, uint32_t len);

/* kernel/fs/ramfs.c — does NOT call user_ptr_valid */
int ramfs_populate(ramfs_t *inst, const char *name, const uint8_t *kbuf, uint32_t len) {
    ramfs_file_t *f = rfs_find(inst, name);
    if (!f) { f = rfs_alloc(inst, name); if (!f) return -12; }
    if (len == 0) { f->size = 0; return 0; }  /* empty slot */
    if (!f->data) { f->data = (uint8_t *)kva_alloc_pages(1); if (!f->data) return -12; }
    if (len > RAMFS_MAX_SIZE) len = RAMFS_MAX_SIZE;
    __builtin_memcpy(f->data, kbuf, len);
    f->size = len;
    return 0;
}
```

`ramfs_write_fn` (called from userspace via `sys_write`) retains its `user_ptr_valid` check unchanged.

**`ramfs_opendir` — for `getdents64` on `/etc` and `/root`:**

File ops and directory ops use different vtables because `priv` points to different types (`ramfs_file_t *` for files, `ramfs_t *` for directories):

```c
/* Directory readdir — priv is ramfs_t *, not ramfs_file_t * */
static int ramfs_readdir_fn(void *priv, uint64_t index, char *name_out, uint8_t *type_out) {
    ramfs_t *inst = (ramfs_t *)priv;
    uint64_t found = 0;
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!inst->files[i].in_use) continue;
        if (found == index) {
            rfs_strcpy(name_out, inst->files[i].name, RAMFS_MAX_NAMELEN);
            *type_out = 8;  /* DT_REG */
            return 0;
        }
        found++;
    }
    return -1;
}

/* Directory vtable — read/write return EISDIR; only readdir is meaningful */
static const vfs_ops_t s_ramfs_dir_ops = {
    .read    = NULL,   /* EISDIR */
    .write   = NULL,   /* EISDIR */
    .close   = ramfs_close_fn,
    .readdir = ramfs_readdir_fn,   /* uses ramfs_t * priv */
    .dup     = (void *)0,
    .stat    = NULL,
};

/* File vtable — readdir is meaningful only for directory handles */
static const vfs_ops_t s_ramfs_ops = {
    .read    = ramfs_read_fn,
    .write   = ramfs_write_fn,
    .close   = ramfs_close_fn,
    .readdir = NULL,              /* files are not directories */
    .dup     = (void *)0,
    .stat    = ramfs_stat_fn,
};

int ramfs_opendir(ramfs_t *inst, vfs_file_t *out) {
    out->ops    = &s_ramfs_dir_ops;
    out->priv   = (void *)inst;   /* ramfs_t *, not ramfs_file_t * */
    out->offset = 0;
    out->size   = 0;
    return 0;
}
```

The existing `s_ramfs_ops` in `ramfs.c` gains a NULL `.readdir` entry (it currently omits `.readdir` from the struct initializer, which is equivalent to NULL in C). The `s_ramfs_dir_ops` is new.

### VFS routing changes

**Critical ordering rule:** The `/etc/` and `/root/` ramfs checks in `vfs_open` and `vfs_stat_path` must come **before** `initrd_open()`. If initrd is checked first, it matches `/etc/motd` (read-only) and the ramfs copy is never reached. The ramfs copy must shadow the initrd copy.

Updated `vfs_open` routing (in order):

```c
int vfs_open(const char *path, int flags, vfs_file_t *out) {

    /* 1. /run/ → run ramfs */
    if (path[0]=='/' && path[1]=='r' && path[2]=='u' && path[3]=='n' && path[4]=='/')
        return ramfs_open(&s_run_ramfs, path + 5, flags, out);

    /* 2. /etc/ → etc ramfs (MUST precede initrd_open to shadow read-only copies) */
    if (path[0]=='/' && path[1]=='e' && path[2]=='t' && path[3]=='c') {
        if (path[4] == '/')
            return ramfs_open(&s_etc_ramfs, path + 5, flags, out);
        if (path[4] == '\0')
            return ramfs_opendir(&s_etc_ramfs, out);  /* open("/etc", O_RDONLY) */
    }

    /* 3. /root/ → root ramfs */
    if (path[0]=='/' && path[1]=='r' && path[2]=='o' && path[3]=='o' && path[4]=='t') {
        if (path[5] == '/')
            return ramfs_open(&s_root_ramfs, path + 6, flags, out);
        if (path[5] == '\0')
            return ramfs_opendir(&s_root_ramfs, out);
    }

    /* 4. initrd — /bin/, /dev/, etc. (no longer matches /etc/) */
    if (initrd_open(path, out) == 0) return 0;

    /* 5. ext2 fallback (when disk is present) */
    ...
}
```

**`vfs_stat_path` routing changes** (same ordering rule — ramfs before initrd):

```c
int vfs_stat_path(const char *path, k_stat_t *out) {
    /* Directory stubs for /, /bin, /dev, /run, /etc, /root */
    if (streq(path, "/") || streq(path, "/bin") || streq(path, "/dev") ||
        streq(path, "/run") || streq(path, "/etc") || streq(path, "/root")) {
        /* ... S_IFDIR stub ... */
        return 0;
    }

    /* /run/ files → run ramfs */
    if (path[0]=='/' && path[1]=='r' && path[2]=='u' && path[3]=='n' && path[4]=='/')
        return ramfs_stat(&s_run_ramfs, path + 5, out);

    /* /etc/ files → etc ramfs (MUST precede initrd to shadow read-only copies) */
    if (path[0]=='/' && path[1]=='e' && path[2]=='t' && path[3]=='c' && path[4]=='/')
        return ramfs_stat(&s_etc_ramfs, path + 5, out);

    /* /root/ files → root ramfs */
    if (path[0]=='/' && path[1]=='r' && path[2]=='o' && path[3]=='o' && path[4]=='t' && path[5]=='/')
        return ramfs_stat(&s_root_ramfs, path + 6, out);

    /* /dev/ specials */
    /* ... existing dev handling ... */

    /* initrd — /bin/ and anything not matched above */
    if (initrd_stat_entry(path, out) == 0) return 0;

    /* ext2 fallback */
    /* ... existing ext2 handling ... */
}
```

### What this fixes

- `touch /etc/foo`, `mkdir /etc/x` — works (ramfs backed)
- `touch /root/.profile` — works
- `cat /etc/motd` — still works (reads from ramfs copy, populated from initrd at boot)
- `ls /etc/` via `getdents64` — works via `ramfs_opendir`
- `/etc/vigil/services/` — all vigil service files readable and writable

### Slot sizing and constant migration

`RAMFS_MAX_FILES` moves from a `#define` in `ramfs.c` to `ramfs.h` (value bumped from 16 to 32 per instance). **Remove the existing `#define RAMFS_MAX_FILES 16` from `ramfs.c`** — the canonical definition is now in `ramfs.h` only.

32 slots per instance is sufficient: `/etc` initrd has ~20 files; `/root` starts empty. The 4096-byte per-file limit is fine: all `/etc` files are small (vigil service configs, shadow, passwd, motd all < 512 bytes). CA bundle goes to ext2, not ramfs.

---

## Part 2 — net_init() Changes

Remove from `net_init()`:
- `net_set_config(0x0a00020f, 0xffffff00, 0x0a000202)` — static IP call
- ICMP echo request construction, send, and reply poll loop
- `[NET] configured:`, `[NET] ICMP: echo request sent`, `[NET] ICMP: echo reply from` printk lines
- `s_ping_buf` static buffer (no longer needed)
- `s_icmp_reply_received` flag (no longer needed)

Keep in `net_init()`:
- Early return if `netdev_get("eth0") == NULL`
- `eth_init()`, `udp_init()`, `tcp_init()`

After this change, `net_init()` emits no serial output when eth0 is present. The NIC is initialized but has no IP. The DHCP daemon configures it after the scheduler starts.

`tests/expected/boot.txt` is **unaffected** — `-machine pc` has no eth0; `net_init()` returns at the early check; no lines change.

---

## Part 3 — DHCP + DNS Daemon

### Vigil extension: NET_ADMIN cap

The vigil `service_t` struct and `start_service()` currently support two cap types (`AUTH`, `NET_SOCKET`). The DHCP daemon needs `CAP_KIND_NET_ADMIN = 8u` granted with **write rights** (value 2), because `sys_netcfg` op=0 calls `cap_check(..., CAP_KIND_NET_ADMIN, CAP_RIGHTS_WRITE)`.

**Changes to `user/vigil/main.c`:**

```c
/* Add constant */
#define SVC_CAP_NET_ADMIN   8u
#define SVC_CAP_RIGHTS_WRITE 2u

/* Add field to service_t */
typedef struct {
    ...
    int needs_net_admin;
    ...
} service_t;

/* In load_service(), after needs_net_socket line: */
s->needs_net_admin = (strstr(caps_buf, "NET_ADMIN") != NULL);

/* In start_service(), after needs_net_socket grant: */
if (s->needs_net_admin)
    syscall(361, (long)SVC_CAP_NET_ADMIN, (long)SVC_CAP_RIGHTS_WRITE);
```

### Daemon: user/dhcp/main.c

Single musl-static binary (~350 lines). No dynamic allocation.

#### DHCP packet layout

Standard BOOTP fixed header (236 bytes) + magic cookie (4 bytes) + options. Key fields:

| Field | Value |
|-------|-------|
| `op` | 1 (BOOTREQUEST) |
| `htype` | 1 (Ethernet) |
| `hlen` | 6 |
| `xid` | lower 4 bytes of MAC, incremented per transaction |
| `flags` | `htons(0x8000)` — **broadcast flag required on real hardware** |
| `chaddr` | MAC from `sys_netcfg(1, &info)` |
| magic cookie | `0x63825363` (network byte order) |

#### sys_netcfg usage

```c
/* Read MAC (op=1) — returned ip/mask/gateway are in network byte order as-is */
struct { uint8_t mac[6]; uint8_t pad[2];
         uint32_t ip; uint32_t mask; uint32_t gateway; } info;
syscall(500, 1, (long)&info, 0, 0);

/* Set IP/mask/gw (op=0) — pass values in network byte order */
/* DHCP option values are already in network byte order; pass them directly */
syscall(500, 0, (long)ip_nbo, (long)mask_nbo, (long)gw_nbo);
```

`ip_nbo`, `mask_nbo`, `gw_nbo` are the raw values extracted from DHCP options. DHCP options are already in network byte order. Do **not** call `htonl()` on them before passing to `sys_netcfg` — they are correct as-is. `net_set_config` stores them directly without byte-swapping.

#### Options sent in DISCOVER and REQUEST

- Option 53 (message type): 1=DISCOVER, 3=REQUEST
- Option 55 (parameter request list): `{1, 3, 6, 51}` (mask, router, DNS, lease time)
- Option 61 (client identifier): `\x01` + 6-byte MAC
- Option 255 (end)

#### State machine

```
INIT
  │
  ▼
SELECTING ─── DISCOVER broadcast (src 0.0.0.0:68 → dst 255.255.255.255:67)
  │             500ms recv timeout per attempt; retry DISCOVER on timeout
  │ OFFER received (option 53 = 2)
  ▼
REQUESTING ── REQUEST broadcast (echo offered IP in option 50 + server ID from option 54)
  │             500ms recv timeout per attempt; retry REQUEST on timeout
  │ ACK received (option 53 = 5)
  ▼
BOUND ──────── syscall(500, 0, ip_nbo, mask_nbo, gw_nbo)
               write /etc/resolv.conf if DNS server present
               print [DHCP] acquired X.X.X.X/P gw G.G.G.G lease Ns dns D.D.D.D
               sleep T1 = lease_time / 2
  │
  ▼
RENEWING ───── DHCPREQUEST unicast to server_ip (not broadcast — RFC 2131 §4.4.5)
  │             on ACK: update lease, print [DHCP] renewed ..., sleep new T1
  │             on NAK or timeout: fall back to SELECTING (new DISCOVER cycle)
  ▼ (loop)
```

#### Exponential backoff

A "transaction failure" is: no OFFER after 3 DISCOVER retries, or no ACK after 3 REQUEST retries. On failure:

| Attempt | Wait before retry |
|---------|------------------|
| 1 | 4s |
| 2 | 8s |
| 3 | 16s |
| 4 | 32s |
| 5 | 32s → exit |

After 5 failed transactions: print `[DHCP] failed after 5 attempts, exiting` and `_exit(1)`. Vigil respawns via `max_restarts=10`.

#### Options parsed from OFFER/ACK

| Option | Field | Default if absent |
|--------|-------|-------------------|
| 1 | Subnet mask | `0xffffff00` (255.255.255.0) |
| 3 | Router (gateway) | `0x00000000` |
| 6 | DNS server (first IP only) | none |
| 51 | Lease time (seconds) | 3600 |
| 54 | Server identifier | source IP of OFFER |

All other options ignored.

#### DNS — /etc/resolv.conf

After BOUND, if option 6 was present in the ACK:
```c
/* /etc/resolv.conf is ramfs-backed (writable) from Part 1 */
int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
dprintf(fd, "nameserver %u.%u.%u.%u\n", dns[0], dns[1], dns[2], dns[3]);
close(fd);
```

musl's `getaddrinfo` reads `/etc/resolv.conf` automatically. No kernel changes needed for DNS.

If option 6 is absent, `/etc/resolv.conf` is left as the empty file created at boot (musl resolver returns `EAI_AGAIN` for hostnames; IP-based connections still work).

#### Serial output

```
[DHCP] acquired 10.0.2.15/24 gw 10.0.2.2 lease 86400s dns 10.0.2.3
[DHCP] renewed lease, next renewal in 43200s
[DHCP] failed after 5 attempts, exiting
```

### Vigil service: /etc/vigil/services/dhcp/

| File | Content |
|------|---------|
| `run` | `exec /bin/dhcp` |
| `policy` | `respawn\nmax_restarts=10` |
| `caps` | `NET_ADMIN NET_SOCKET` |
| `user` | `root` |

**Ordering:** `dhcp` sorts before `getty` and `httpd` alphabetically. Vigil iterates services in directory order. DHCP starts first. httpd binds `0.0.0.0:80` (not IP-specific), so there is no ordering hazard.

### Makefile additions

- `user/dhcp/Makefile` — `musl-gcc -static -O2 -o dhcp main.c`
- Initrd build: embed `/bin/dhcp`, create `/etc/vigil/services/dhcp/` with four service files
- No new kernel source files needed beyond `ramfs.c`/`vfs.c` changes and `ip.c` edits

---

## Part 4 — BearSSL + curl

**Read in conjunction with:** `docs/superpowers/specs/2026-03-25-phase26-6-bearssl-curl-design.md` — that document contains the complete build scripts (`tools/fetch-bearssl.sh`, `tools/build-bearssl.sh`, `tools/build-curl.sh`) and Makefile targets verbatim. Implement Part 4 using those scripts exactly as written. Summary of what they do:

- BearSSL 0.6 fetched to `references/bearssl-0.6/` via `tools/fetch-bearssl.sh`, compiled to `build/bearssl-install/lib/libbearssl.a` via `tools/build-bearssl.sh`
- curl 8.13.0 (source in `references/curl/`) built via VPATH in `build/curl-build/` with `musl-gcc -static --with-bearssl --without-openssl`
- Binary stripped, output to `build/curl/curl`; embedded into ext2 disk at `/bin/curl` via `make disk`
- CA bundle (`tools/cacert.pem`, ~220 KB, committed) embedded into ext2 at `/etc/ssl/certs/ca-certificates.crt`

**`/etc/ssl/certs/` on ext2, not ramfs.** The CA bundle is ~220 KB — too large for the 4 KB-per-file ramfs limit. It lives on the ext2 disk image only. `make disk` creates the directory and copies it in.

**curl is ext2-only.** No curl binary in the initrd. A live ISO boot without the NVMe disk has no `/bin/curl`. `make test` is unaffected.

**DNS path for curl:**
```
musl getaddrinfo("example.com")
  → reads /etc/resolv.conf → nameserver 10.0.2.3
  → UDP DNS query (SO_RCVTIMEO=5s already implemented in Phase 26)
  → SLIRP DNS at 10.0.2.3 forwards to host DNS
  → TCP to port 443
  → BearSSL TLS (validates /etc/ssl/certs/ca-certificates.crt from ext2)
  → HTTP/1.1 GET → stdout
```

---

## Testing

### make test / boot.txt

Unaffected. `-machine pc` has no eth0; `net_init()` returns silently; no `[DHCP]` lines; `boot.txt` unchanged.

### test_net_stack.py — updated

- Boot with `INIT=vigil` (DHCP daemon runs as vigil service)
- **Remove** expectation of `[NET] configured: 10.0.2.15/24 gw 10.0.2.2`
- **Remove** expectation of `[NET] ICMP: echo reply from 10.0.2.2`
- **Add** expectation: wait for line matching `[DHCP] acquired` (any IP, not hardcoded to 10.0.2.15)
- PASS when line appears within timeout

### test_curl.py — new, added to run_tests.sh

- Boot q35 + virtio-net + SLIRP + disk image (ext2 with curl + CA bundle)
- Wait for `[DHCP] acquired`
- Send shell command: `curl -s https://example.com | head -5`
- PASS when output contains `<!doctype html>` (case-insensitive)

### test_socket.py

Unchanged. httpd vigil service unaffected; DHCP daemon configures network before httpd serves requests.

---

## File Layout Summary

| File | Action |
|------|--------|
| `kernel/fs/ramfs.h` | Modify — add `ramfs_t` struct, parameterize all APIs |
| `kernel/fs/ramfs.c` | Modify — thread `ramfs_t *` through all functions + `ramfs_opendir` |
| `kernel/fs/vfs.c` | Modify — three ramfs instances, `/etc/`+`/root/` routing (before initrd), `vfs_init` population |
| `kernel/net/ip.c` | Modify — remove static IP, ICMP test, and related printks from `net_init()` |
| `user/vigil/main.c` | Modify — add `needs_net_admin`, `SVC_CAP_NET_ADMIN=8`, `SVC_CAP_RIGHTS_WRITE=2`, grant in `start_service()` |
| `user/dhcp/main.c` | Create — DHCP client daemon (~350 lines) |
| `user/dhcp/Makefile` | Create — musl-gcc build |
| `Makefile` | Modify — add dhcp target, vigil service dirs, update initrd |
| `tools/fetch-bearssl.sh` | Create — download BearSSL 0.6 |
| `tools/build-bearssl.sh` | Create — compile BearSSL with musl-gcc |
| `tools/build-curl.sh` | Create — configure + compile curl |
| `tools/cacert.pem` | Create + Commit — Mozilla CA bundle |
| `tests/test_net_stack.py` | Modify — INIT=vigil, wait for [DHCP] acquired |
| `tests/test_curl.py` | Create — q35 + disk + HTTPS smoke test |

---

## Forward Constraints

- **`/etc` ramfs is not persisted.** Runtime changes (new vigil services, updated configs) are lost on reboot. Phase 28 (writable root) addresses this.
- **`/root` ramfs is not persisted.** Shell history, dotfiles lost on reboot. Phase 28.
- **curl is ext2-only.** Disk required. Live ISO boot has no curl.
- **No DHCPINFORM, DHCPDECLINE, DHCPRELEASE.** Minimal RFC 2131.
- **Single DNS server.** First IP from option 6 only.
- **No search domains.** Option 15 ignored.
- **No IPv6 / DHCPv6.** IPv4 only.
- **HTTP/2 disabled.** `--without-nghttp2`. curl uses HTTP/1.1.
- **No content-encoding.** `--without-zlib --without-brotli --without-zstd`.
- **DHCP renewal uses `nanosleep` which busy-waits.** `T1 = lease_time / 2` (typically 43200s). Until `sys_nanosleep` is fixed to call `sched_block` (Phase 28), the DHCP daemon busy-waits during the renewal sleep, consuming scheduler time. Acceptable in QEMU tests (short lease times); must be fixed before real hardware deployment.
- **`sys_netcfg` is Aegis-specific.** No `ioctl(SIOCSIFADDR)` / rtnetlink compatibility.
