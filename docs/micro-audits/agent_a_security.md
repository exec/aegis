# Security Micro-Audits — Agent A

Date: 2026-04-01
Auditor: Claude Opus 4.6 (automated)
Scope: 5 targeted audits of Aegis kernel security subsystems

---

## A1. Exec Baseline Over-Grant

**Severity: HIGH**

### Affected Files

- `kernel/syscall/sys_process.c` lines 941-972 (execve baseline)
- `kernel/syscall/sys_process.c` lines 1916-1924 (sys_spawn baseline)
- `kernel/proc/proc.c` lines 377-481 (init/proc_spawn grants)

### Findings

The execve baseline (applied to EVERY binary after exec) grants 8 capability kinds. The init process (proc_spawn) grants 15. The table below maps each baseline cap to its actual consumers.

#### Execve Baseline (every binary gets these)

| Cap Kind | Rights | Actual Consumers (cap_check sites) | Recommendation |
|----------|--------|------------------------------------|----------------|
| `VFS_OPEN` (1) | READ | `sys_open` | **Keep** -- needed by all binaries |
| `VFS_WRITE` (2) | WRITE | `sys_write` | **Keep** -- needed by all binaries |
| `VFS_READ` (3) | READ | `sys_read` | **Keep** -- needed by all binaries |
| `NET_SOCKET` (7) | READ | `sys_socket` (AF_INET) | **Remove from baseline.** Only httpd, curl, dhcp need network sockets. Move to capd-only. A sandboxed text editor or grep should not be able to open network sockets. |
| `THREAD_CREATE` (9) | READ | `sys_clone` (CLONE_VM) | **Remove from baseline.** Only dynamically-linked binaries using pthreads need this. Simple utilities (cat, echo, ls) never create threads. Move to capd-only. |
| `PROC_READ` (10) | READ+WRITE | `sys_kill`, `sys_setfg`, procfs reads | **Split.** READ for /proc access can stay in baseline. WRITE (used for kill/setfg) should be capd-only -- arbitrary signal sending is a privilege. |
| `FB` (12) | READ | `sys_fb_map` | **Remove from baseline.** Only lumen (compositor) maps the framebuffer. Every other binary getting FB access is a security hole -- a malicious binary could overwrite the display. |
| `IPC` (15) | READ | `sys_socket` (AF_UNIX), `sys_memfd_create` | **Keep conditionally.** AF_UNIX is needed for capd communication. However, memfd_create + MAP_SHARED gives arbitrary shared memory. Consider splitting IPC into IPC_UNIX and IPC_MEMFD. |

#### Init-only grants (NOT in execve baseline -- correct)

| Cap Kind | Consumer | Status |
|----------|----------|--------|
| `AUTH` (4) | `sys_open("/etc/shadow")` | Correctly restricted to init->vigil->login chain |
| `SETUID` (6) | `sys_setuid`/`sys_setgid` | Correctly restricted |
| `NET_ADMIN` (8) | `sys_netcfg` | Correctly restricted to DHCP daemon |
| `DISK_ADMIN` (11) | `sys_blkdev_io/list/gpt_rescan` | Correctly restricted to installer |
| `CAP_GRANT` (5) | `sys_cap_grant_exec` | Correctly restricted |
| `CAP_DELEGATE` (13) | `sys_cap_grant_runtime` | Correctly restricted |
| `CAP_QUERY` (14) | `sys_cap_query` | Correctly restricted |

### Recommendation

Remove `NET_SOCKET`, `THREAD_CREATE`, `FB`, and `PROC_READ(WRITE)` from the execve baseline. These should be granted via capd policy files to specific binaries that need them. The current baseline gives every binary on the system the ability to open network connections, map the framebuffer, create threads, and send signals to other processes.

---

## A2. Signal Delivery Register Corruption (C5)

**Severity: HIGH**

### Affected Files

- `kernel/signal/signal.c` lines 353-382 (signal_deliver_sysret -- x86-64 SYSRET path)
- `kernel/signal/signal.c` lines 194-293 (signal_deliver -- x86-64 ISR/iretq path)

### Findings

**Confirmed: the SYSRET signal delivery path saves only 6 registers into the signal frame.**

At line 360-365, `signal_deliver_sysret` saves:
```c
sf.gregs[REG_R8]     = (int64_t)frame->r8;
sf.gregs[REG_R9]     = (int64_t)frame->r9;
sf.gregs[REG_R10]    = (int64_t)frame->r10;
sf.gregs[REG_RIP]    = (int64_t)frame->rip;
sf.gregs[REG_EFL]    = (int64_t)frame->rflags;
sf.gregs[REG_RSP]    = (int64_t)frame->user_rsp;
```

**NOT saved:** rbx, rbp, r12, r13, r14, r15, rax, rcx, rdx, rsi, rdi.

The ISR path (`signal_deliver`, lines 247-264) correctly saves ALL registers because it has the full `cpu_state_t` from the ISR push sequence. The bug is SYSRET-path-only.

### Impact

When a signal is delivered on the SYSRET path (i.e., a signal arrives while a syscall is returning), the signal handler executes, then `sigreturn` restores only the 6 saved registers. The callee-saved registers (rbx, rbp, r12-r15) that the interrupted code expected to survive the syscall will contain whatever the signal handler left in them.

**Concrete scenario:** A program with a SIGCHLD handler (e.g., oksh's `j_sigchld`) gets a signal delivered on the SYSRET path. The handler modifies rbx (a callee-saved register from the signal handler's perspective, but it was live in the interrupted code). After sigreturn, the interrupted code resumes with a corrupted rbx. This causes silent data corruption or a crash.

**Why this hasn't been catastrophic yet:** Most signals are delivered on the ISR path (timer interrupt preempts user code). The SYSRET path only fires when a signal is pending at the moment a syscall returns. Additionally, musl's SIGCHLD handler is simple enough that GCC may not clobber callee-saved registers.

### Affected user programs with sigaction handlers

- `oksh` (shell) -- SIGCHLD handler for job control
- `stsh` (Styx shell) -- SIGCHLD handler
- `lumen` (compositor) -- SIGUSR1/SIGUSR2 handlers
- `bastion` (display manager) -- SIGUSR1 handler
- Any program using musl's `signal()` wrapper

### Recommended Fix

The `syscall_frame_t` structure does not contain callee-saved registers because the SYSCALL entry path does not push them (they are preserved by the C ABI across the syscall dispatch function). The fix requires expanding the SYSRET signal delivery to:

1. Save all general-purpose registers into the signal frame's `gregs[]` array. This requires either:
   - (a) Expanding `syscall_frame_t` to include callee-saved registers, pushed in `syscall_entry.asm` before the C call, or
   - (b) Having `signal_deliver_sysret` read the callee-saved registers directly from the current CPU state (they are still in the actual registers at signal delivery time since signal delivery happens inside the syscall dispatch C function, which preserves them).

Option (b) is simpler: at the point `signal_deliver_sysret` is called, the callee-saved registers still hold the user-mode values (the C ABI guarantees syscall_dispatch preserves them). Add inline assembly to read rbx, rbp, r12-r15 and store them in the frame.

**Caveat:** The compiler may use rbx/rbp/r12-r15 for its own purposes within signal_deliver_sysret. The reads must happen at the very start of the function (before the compiler's prologue clobbers them) or be done in the assembly stub in `syscall_entry.asm` before the C call. The assembly approach is more reliable: push callee-saved registers onto the stack in `syscall_entry.asm` and pass them to signal_deliver_sysret as additional parameters or through the frame struct.

---

## A3. TOCTOU in Path Resolution

**Severity: MEDIUM**

### Affected Files

- `kernel/syscall/sys_file.c` lines 26-43 (sys_open path copy)
- `kernel/syscall/sys_process.c` ~line 850-870 (sys_execve path copy)
- `kernel/fs/ext2.c` lines 458-629 (ext2_open_ex path walk)

### Findings

#### Path copy is safe against CLONE_VM threads

`sys_open` (line 32-43) copies the path byte-by-byte from userspace into a kernel buffer `kpath[256]` before any resolution. This copy is atomic with respect to TOCTOU -- after copy, the path is a kernel-owned string. A CLONE_VM thread cannot modify the kernel copy.

**However:** The byte-by-byte copy itself has a micro-TOCTOU window. Between copying byte N and byte N+1, a CLONE_VM thread could modify earlier bytes of the path in the shared address space. Since `copy_from_user` copies one byte at a time in a loop, a concurrent writer could create a path that was never the actual string at any point in time. In practice, this requires a race between two threads on the same byte, which is extremely tight. The kernel uses the copied path, so the resolved path is at least a consistent string (whatever was read). This is the same behavior as Linux's `strncpy_from_user`.

**Verdict:** The copy-then-use pattern is correct. The theoretical micro-TOCTOU is inherent to any non-atomic string copy from shared memory and is not exploitable for privilege escalation.

#### ext2_open_ex 512-byte resolved buffer

The `resolved[512]` buffer in `ext2_open_ex` (line 463) is a stack-local buffer. The path passed to it is already the kernel-copied `kpath` -- not a user pointer. Symlink targets are read from disk (ext2 inodes), not from userspace. So there is no TOCTOU issue with the resolved buffer itself.

**Potential concern:** The resolved buffer is 512 bytes. A chain of symlinks could construct a path longer than 511 characters via concatenation of symlink targets (line 603-613). The loop checks `np < 510` preventing overflow. A path that exceeds 510 bytes is silently truncated, which could resolve to the wrong file. This is not a TOCTOU issue but a correctness issue -- truncation should return `-ENAMETOOLONG`.

#### Shadow path check is pre-resolution (see A4)

The `/etc/shadow` check in `sys_open` (lines 78-86) compares against the user-supplied path BEFORE symlink resolution. This is a bypass vector -- see A4.

### Recommendation

1. The TOCTOU risk in path copy is inherent and acceptable (matches Linux).
2. The 512-byte truncation should return `-ENAMETOOLONG` instead of silently truncating.
3. The shadow path capability check must be applied after symlink resolution, not before. See A4.

---

## A4. /etc/shadow Accessibility

**Severity: CRITICAL**

### Affected Files

- `kernel/syscall/sys_file.c` lines 74-88 (shadow gate in sys_open)
- `kernel/fs/initrd.c` line 209 (initrd stat mode 0555)
- `kernel/fs/initrd.c` line 142 (shadow in initrd file table)
- `kernel/cap/cap.h` line 21 (CAP_KIND_AUTH definition)

### Findings

#### The capability gate

`sys_open` checks `CAP_KIND_AUTH` when the path is exactly `/etc/shadow` (line 78-86). This is the ONLY check preventing unauthorized reads of the password database.

#### Bypass 1: Symlink traversal

If ext2 is mounted (which it is on installed systems), a user process can:

1. Create a symlink: `ln -s /etc/shadow /tmp/myshadow`
2. Open `/tmp/myshadow`

The `sys_open` capability check runs on the string `/tmp/myshadow`, which does NOT match `/etc/shadow`. The check passes without CAP_KIND_AUTH. Then `vfs_open` resolves the symlink via `ext2_open_ex` and opens the actual shadow file.

**Status:** This bypass works today on any installed system with ext2. The initrd-only boot path is not affected (initrd has no symlinks), but installed Aegis systems are vulnerable.

#### Bypass 2: Path traversal with `..` components

The check is an exact byte comparison against `/etc/shadow`. The path `/etc/../etc/shadow` would NOT match the literal string `/etc/shadow`, but would resolve to the same file via ext2_open_ex's `..` handling. Similarly, double slashes like `/etc//shadow` would not match the literal but may resolve identically.

#### Bypass 3: Relative path edge case

`sys_open` resolves relative paths against `proc->cwd` before the shadow check (lines 48-67). So `./etc/shadow` from cwd `/` becomes `/etc/shadow` after resolution. This specific case is handled. However, paths involving `..` or symlinks in the CWD resolution are not normalized.

#### Mode bits

The initrd `initrd_stat_fn` returns mode `S_IFREG | 0555` (line 209) for ALL files including `/etc/shadow`. This means shadow is world-readable at the VFS stat level. The ext2 copy of shadow (written by the installer) may have correct 0640 permissions, but the initrd copy does not.

DAC enforcement in `vfs.c` (ext2 path only) would block unauthorized reads on the ext2 copy. But the initrd copy is checked first in the VFS open order (`initrd first -> ext2 fallback`), so even with correct ext2 permissions, the initrd copy of shadow is always accessible to anyone with VFS_OPEN.

### Recommendation

1. **Move the CAP_KIND_AUTH check into the VFS layer** (after symlink resolution), not in sys_open before resolution. Alternatively, check the resolved path/inode in `ext2_open_ex` or `vfs_open`.
2. **Normalize paths before checking.** Resolve `.`, `..`, and double slashes before comparing against protected paths. Or maintain a set of protected inodes and check after resolution.
3. **Fix initrd mode bits for /etc/shadow.** Use `S_IFREG | 0640` or `0600` and enforce DAC in the initrd path too.
4. **Defense in depth:** Check the capability against the final resolved inode/path, not the user-supplied string.

---

## A5. Capability Delegation Chain Integrity

**Severity: MEDIUM**

### Affected Files

- `kernel/syscall/sys_process.c` lines 1408-1421 (sys_cap_grant_exec -- syscall 361)
- `kernel/syscall/sys_process.c` lines 1430-1462 (sys_cap_grant_runtime -- syscall 363)
- `kernel/cap/src/lib.rs` lines 49-77 (cap_grant Rust implementation)

### Findings

#### Can a process grant a cap it does not hold?

**sys_cap_grant_exec (361):** Checks only that the caller holds `CAP_KIND_CAP_GRANT` (line 1411-1412). It does **NOT** check whether the caller holds the specific cap kind being granted. A process with CAP_GRANT can pre-register ANY cap kind (AUTH, DISK_ADMIN, NET_ADMIN, etc.) in its exec_caps table for the next execve. This is a **privilege escalation vulnerability**.

Example attack: vigil holds CAP_GRANT. It can call `sys_cap_grant_exec(CAP_KIND_DISK_ADMIN, READ|WRITE)` even though vigil does not hold DISK_ADMIN. The next child gets DISK_ADMIN.

In practice, vigil is trusted (PID 1), so this is currently safe. But the principle is violated -- the check should verify the caller holds the cap being delegated.

**sys_cap_grant_runtime (363):** Correctly checks TWO conditions:
1. Caller holds `CAP_KIND_CAP_DELEGATE` (line 1435-1437)
2. Caller holds the specific cap kind being granted (line 1448-1449)

However, the rights check on condition 2 passes `0` as the rights parameter (line 1448: `cap_check(caller->caps, CAP_TABLE_SIZE, kind, 0)`). This means a caller with `CAP_KIND_AUTH | READ` can grant `CAP_KIND_AUTH | READ|WRITE|EXEC` to the target. **The caller can escalate the rights bits beyond what it holds.**

#### CAP_TABLE_SIZE exhaustion

`cap_grant` in Rust (lib.rs line 76) returns `-(ENOCAP)` when all 64 slots are full. `sys_cap_grant_runtime` (line 1458) returns `-ENOSPC` (28) to userspace. `sys_cap_grant_exec` (line 1419) returns `-ENOMEM` (12).

**This is correct behavior** -- the table is full and the operation fails gracefully. However, a malicious capd could fill a target's cap table with garbage entries (64 calls to sys_cap_grant_runtime with distinct kinds 1 through 63), preventing the target from receiving legitimate caps. This is a denial-of-service on the capability system.

Note: `cap_grant` scans for `kind == 0` (empty slot). It does not deduplicate -- granting the same cap kind twice uses two slots. This means capd could waste slots by granting duplicate caps.

#### kind validation too permissive

Both syscalls validate `kind >= CAP_TABLE_SIZE` (i.e., kind >= 64) returns EINVAL (lines 1416, 1442). Since the highest defined kind is `CAP_KIND_POWER = 16`, this allows granting undefined cap kinds 17-63. These would pass `cap_check` for any future syscall checking those kinds. The validation should use a `MAX_CAP_KIND` constant (currently 16) instead of `CAP_TABLE_SIZE` (64).

### Recommendation

1. **Fix sys_cap_grant_exec:** Add a check that the caller holds the specific cap kind being delegated (match sys_cap_grant_runtime pattern).
2. **Fix sys_cap_grant_runtime rights escalation:** Change line 1448 from `cap_check(caller->caps, ..., kind, 0)` to `cap_check(caller->caps, ..., kind, rights)` to ensure the caller holds at least the rights being granted.
3. **Add a MAX_CAP_KIND constant** and validate `kind <= MAX_CAP_KIND` instead of `kind < CAP_TABLE_SIZE`.
4. **Consider deduplication** in cap_grant -- if a slot with the same kind already exists, merge rights instead of consuming a new slot. This prevents slot exhaustion attacks.

---

## Summary

| Audit | Severity | Status | Core Issue |
|-------|----------|--------|------------|
| A1. Exec baseline over-grant | HIGH | Open | FB, NET_SOCKET, THREAD_CREATE, PROC_READ(WRITE) granted to all binaries |
| A2. Signal register corruption | HIGH | ✅ Fixed (836f480) | SYSRET path signal delivery corrupts rbx/rbp/r12-r15 |
| A3. TOCTOU in path resolution | MEDIUM | Open | ext2_open_ex truncation; shadow bypass via symlink (see A4) |
| A4. /etc/shadow accessible | CRITICAL | ✅ Fixed (fc7cdff) | CAP_KIND_AUTH check bypassed via symlink or `..` path |
| A5. Capability delegation | MEDIUM | Open | sys_cap_grant_exec lacks "holds cap" check; rights escalation in runtime grant |
