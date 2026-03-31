# Phase 45: capd + Capability Helpers — Design Spec

**Date:** 2026-03-31
**Status:** Approved
**Depends on:** Phase 44 (IPC — AF_UNIX, SO_PEERCRED), Phase 42 (stsh)

---

## Overview

Phase 45 introduces runtime capability delegation via a new syscall (`sys_cap_grant`)
and a userspace capability broker daemon (`capd`). This moves capability policy out of
Vigil's fork/exec path into a dedicated, replaceable service. Services request
capabilities at runtime over AF_UNIX; capd validates requests against declarative
policy files using kernel-attested peer credentials (SO_PEERCRED).

### Goals

1. **sys_cap_grant syscall** — grant a capability to a running process by PID
2. **capd daemon** — AF_UNIX socket broker with per-binary policy files
3. **Vigil simplification** — remove exec_caps from all services except capd
4. **stsh `grant` builtin** — interactive operator capability delegation
5. **Service migration** — login, dhcp, lumen request caps from capd at startup

---

## Component 1: sys_cap_grant Syscall

**Syscall number:** 363 (next after sys_cap_query at 362)

**Signature:**
```c
uint64_t sys_cap_grant(uint64_t target_pid, uint64_t kind, uint64_t rights);
```

**Kernel enforcement:**
- Caller must hold `CAP_KIND_CAP_DELEGATE` (kind 13) with `CAP_RIGHTS_READ`
- Caller must hold the specific capability being granted (matching kind, with at
  least the requested rights)
- `target_pid` must refer to an existing process (`proc_find_by_pid`)
- `kind` must be 1–15 (non-null, within range)
- Target's cap table must have a free slot

**Returns:** Slot index (0–15) on success. Negative errno on failure:
- `-ENOCAP` (130): caller lacks CAP_DELEGATE or the requested cap
- `-ESRCH` (3): target PID not found
- `-EINVAL` (22): invalid kind (0 or >= CAP_TABLE_SIZE)
- `-ENOSPC` (28): target cap table full

**Implementation:** `sys_process.c`, using `proc_find_by_pid()` + existing
`cap_check`/`cap_grant` Rust FFI. Holds sched_lock for the duration of the
find + grant to prevent the target from exiting between lookup and grant.

---

## Component 2: capd Daemon

**Binary:** `/bin/capd`
**Source:** `user/capd/main.c`

### Startup

1. Load policy files from `/etc/aegis/capd.d/`
2. Create AF_UNIX socket, bind to `/run/capd.sock`
3. Listen with backlog of 8
4. Print `[CAPD] listening on /run/capd.sock` to stderr
5. Accept loop: one connection at a time (single-threaded)

### Policy Files

**Location:** `/etc/aegis/capd.d/<basename>.policy`

One file per binary basename. Format:
```
allow AUTH CAP_GRANT CAP_DELEGATE CAP_QUERY SETUID
```

Single line, `allow` keyword followed by space-separated capability names.

**Policy files for Phase 45:**

| File | Contents |
|------|----------|
| `login.policy` | `allow AUTH CAP_GRANT CAP_DELEGATE CAP_QUERY SETUID` |
| `dhcp.policy` | `allow NET_ADMIN` |
| `lumen.policy` | `allow CAP_GRANT` |

httpd and chronos need no policy files — they operate on baseline caps only.

### Protocol

Text-based, one request per connection:

1. Client connects to `/run/capd.sock`
2. capd calls `getsockopt(SO_PEERCRED)` → client PID, UID, GID
3. capd reads `/proc/<pid>/exe` → client binary path → extract basename
4. Client sends: `GRANT <CAP_NAME>\n` (max 64 bytes)
5. capd validates:
   - Binary basename has a matching `.policy` file
   - Requested cap name is in the policy's allow list
6. capd calls `syscall(363, client_pid, kind, CAP_RIGHTS_READ|CAP_RIGHTS_WRITE|CAP_RIGHTS_EXEC)`
   (full rights — policy file controls which caps, not which rights)
7. capd sends response: `OK\n` or `ERR <reason>\n`
8. capd logs to stderr: `[CAPD] GRANT <cap> -> pid <N> (<binary>) OK` or `DENIED: <reason>`
9. Connection closed

### capd's Own Capabilities

capd is the "cap root" — it holds every delegatable capability so it can grant
any of them per policy. Vigil grants capd these exec_caps:

```
CAP_DELEGATE AUTH CAP_GRANT SETUID NET_ADMIN DISK_ADMIN FB CAP_QUERY THREAD_CREATE
```

Plus baseline caps (VFS_OPEN, VFS_READ, VFS_WRITE, NET_SOCKET, PROC_READ, IPC),
capd has 15 of 16 cap slots filled. The only cap not held is CAP_KIND_NULL (slot 0).

### Error Handling

- Policy file missing: `ERR no policy for <binary>`
- Cap not in policy: `ERR <cap> not allowed for <binary>`
- sys_cap_grant fails: `ERR grant failed: <errno>`
- Invalid request format: `ERR bad request`

---

## Component 3: Vigil Changes

### Remove exec_caps from all services except capd

Currently Vigil reads `caps` files from `/etc/vigil/services/<name>/caps` and
calls `syscall(361, kind, rights)` for each cap before execing the service binary.

**Change:** Vigil keeps this logic but only applies it to capd. All other services'
`caps` files are removed from the ext2 image. The caps parsing code in Vigil
stays (it's generic), but no other service has a caps file to parse.

**capd service descriptor:**
- `/etc/vigil/services/capd/run` → `/bin/capd`
- `/etc/vigil/services/capd/policy` → `respawn`
- `/etc/vigil/services/capd/caps` → `CAP_DELEGATE AUTH CAP_GRANT SETUID NET_ADMIN DISK_ADMIN FB CAP_QUERY THREAD_CREATE`

### Init (Vigil) cap grants

Vigil needs to hold all caps it delegates to capd. `proc_spawn` in `proc.c`
must grant Vigil 15 capabilities (up from 10):

Current 10 + added: `DISK_ADMIN`, `FB`, `THREAD_CREATE`, `CAP_DELEGATE`, `CAP_QUERY`

**boot.txt change:** `[CAP] OK: 10 capabilities granted to init` → `15`

### Service startup ordering

capd must be listening before other services connect. Vigil starts services in
directory iteration order. `capd` sorts before `chronos`, `dhcp`, `getty`, etc.
alphabetically. Services that fail to reach capd retry 3 times with 100ms sleep,
then continue with baseline caps and log a warning.

---

## Component 4: stsh `grant` Builtin

**Usage:** `grant <CAP_NAME> <PID>`

**Implementation:** Direct `syscall(363, pid, kind, CAP_RIGHTS_READ|CAP_RIGHTS_WRITE|CAP_RIGHTS_EXEC)` — no capd roundtrip. Full rights, matching capd behavior.

stsh already holds `CAP_DELEGATE` from the login chain. The kernel enforces that
stsh holds both CAP_DELEGATE and the cap being granted. stsh can only grant caps
it holds (baseline + CAP_DELEGATE + CAP_QUERY).

**Cap name parsing:** Reuses existing `cap_name_to_kind()` in `caps.c`.

**Output:**
- Success: `granted <CAP_NAME> to pid <N>`
- Failure: `grant: <error message>`

**Requires CAP_DELEGATE** — checked via `s_has_delegate` at startup. If not held,
prints `grant: requires CAP_DELEGATE` and returns.

---

## Component 5: Service Client Changes

Services that previously received caps from Vigil exec_caps now request them from
capd at startup. Each service adds a small capd client block before its main loop.

### Client pattern (inline, no library)

```c
static int capd_request(const char *cap_name) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, "/run/capd.sock");
    for (int retry = 0; retry < 3; retry++) {
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            goto connected;
        usleep(100000); /* 100ms */
    }
    close(sock);
    return -1;
connected:
    dprintf(sock, "GRANT %s\n", cap_name);
    char buf[64];
    int n = read(sock, buf, sizeof(buf) - 1);
    close(sock);
    if (n > 0) { buf[n] = 0; return strncmp(buf, "OK", 2) == 0 ? 0 : -1; }
    return -1;
}
```

### Affected services

| Service | Caps to request from capd |
|---------|--------------------------|
| login | AUTH, CAP_GRANT, CAP_DELEGATE, CAP_QUERY, SETUID |
| dhcp | NET_ADMIN |
| lumen | CAP_GRANT |

httpd, chronos: no changes (baseline only).

### Retry logic

Connect to `/run/capd.sock`, retry 3 times with 100ms sleep on ECONNREFUSED.
If all retries fail, continue with baseline caps and print warning to stderr.

---

## Component 6: Testing

### boot.txt update

```
[CAP] OK: 15 capabilities granted to init
```

### test_integrated.py

Add test case: a small `capd_test` binary that:
1. Connects to `/run/capd.sock`
2. Sends `GRANT NET_ADMIN\n`
3. Checks response is `OK\n`
4. Queries own caps via `syscall(362, 0, ...)` to verify NET_ADMIN was granted
5. Prints `CAPD TESTS PASSED`

Policy file: `/etc/aegis/capd.d/capd_test.policy` → `allow NET_ADMIN`

### Existing tests

- Boot oracle: updated cap count (15)
- test_integrated: login must still work (now via capd instead of exec_caps)
- test_socket: unchanged (NET_SOCKET is baseline)

---

## Forward Constraints

1. **No cap revocation.** sys_cap_grant is grant-only. Revocation requires restarting
   the process (capd can request Vigil to restart a service).

2. **capd is single-threaded.** One connection at a time. Adequate for service startup
   (low traffic). Concurrent requests queue on the listen backlog (8).

3. **Full rights granted.** capd grants `CAP_RIGHTS_READ|CAP_RIGHTS_WRITE|CAP_RIGHTS_EXEC`
   for every allowed cap. Per-right policy is deferred.

4. **No wildcard policy.** Each binary needs its own `.policy` file. No `default.policy`
   or glob matching.

5. **capd holds nearly all caps (15/16 slots).** Adding new cap kinds requires expanding
   CAP_TABLE_SIZE. Current value (16) is at capacity.

6. **Policy files are static.** capd loads policy at startup. Changes require capd restart
   (Vigil handles this via `kill -HUP` or service restart).

7. **`/proc/<pid>/exe` identity.** capd trusts the kernel's `/proc/<pid>/exe` for binary
   identification. A process that overwrites its own binary on disk after exec could
   theoretically confuse capd, but this requires write access to `/bin/` which is
   capability-gated.

8. **No challenge-response nonces.** MISSION.md mentions nonces injected at spawn time.
   Deferred — SO_PEERCRED + /proc/pid/exe is sufficient for v1.
