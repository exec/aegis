# Aegis Capability Model

## Principle

No process holds ambient authority. A process starts with exactly the capabilities
it is explicitly granted — nothing more. Not even file access, unless granted.

## What a capability is

A capability is a (kind, rights) pair stored in a per-process kernel table. The
kernel creates capabilities; user space cannot forge or modify them. The table is
embedded in the PCB (`aegis_process_t.caps[]`), inaccessible to user space.

## Capability kinds

Defined in `kernel/cap/cap.h`. CAP_KIND_NULL (0) means an empty slot; the
rest gate specific kernel operations.

| Kind                 | Value | Meaning |
|----------------------|-------|---------|
| CAP_KIND_NULL        | 0     | Empty slot |
| CAP_KIND_VFS_OPEN    | 1     | sys_open |
| CAP_KIND_VFS_WRITE   | 2     | sys_write |
| CAP_KIND_VFS_READ    | 3     | sys_read |
| CAP_KIND_AUTH        | 4     | Read /etc/shadow |
| CAP_KIND_CAP_GRANT   | 5     | Reserved (see `sys_cap_grant_runtime`) |
| CAP_KIND_SETUID      | 6     | sys_setuid / sys_setgid |
| CAP_KIND_NET_SOCKET  | 7     | sys_socket / socket syscalls |
| CAP_KIND_NET_ADMIN   | 8     | sys_netcfg (set IP/mask/gw) |
| CAP_KIND_THREAD_CREATE | 9   | clone with CLONE_VM |
| CAP_KIND_PROC_READ   | 10    | Read /proc/N |
| CAP_KIND_DISK_ADMIN  | 11    | sys_blkdev_io / sys_gpt_rescan |
| CAP_KIND_FB          | 12    | sys_fb_map (framebuffer) |
| CAP_KIND_CAP_DELEGATE | 13   | sys_cap_grant_runtime sender |
| CAP_KIND_CAP_QUERY   | 14    | sys_cap_query |
| CAP_KIND_IPC         | 15    | AF_UNIX / memfd / SCM_RIGHTS |
| CAP_KIND_POWER       | 16    | sys_reboot, sys_sethostname |

## Capability rights

| Right             | Bit | Meaning |
|-------------------|-----|---------|
| CAP_RIGHTS_READ   | 0   | Read access |
| CAP_RIGHTS_WRITE  | 1   | Write access |
| CAP_RIGHTS_EXEC   | 2   | Execute access |

## Cap table layout

`CAP_TABLE_SIZE = 64` slots per process, embedded in the PCB
(`aegis_process_t.caps[64]`). `kind == 0` (CAP_KIND_NULL) means the slot is
empty. Slot ordering is grant-order, not stable; consumers always use
`cap_check(table, n, kind, rights)` to walk for a match.

## Lifecycle

The flow as of Phase 46c (the cap-policy redesign):

1. `proc_spawn` zeros the cap table for init only.
2. **`sys_execve` resets the table on every exec** (cap boundary). Baseline
   caps are unconditionally granted: VFS_OPEN(R), VFS_WRITE(W), VFS_READ(R),
   IPC(R), PROC_READ(R), THREAD_CREATE(R).
3. `cap_policy_lookup(path)` reads `/etc/aegis/caps.d/<basename>` and grants
   each declared cap subject to its tier:
   - **service** caps are granted unconditionally.
   - **admin** caps are granted only if `proc->authenticated == 1`, set by
     `sys_auth_session(364)` after libauth validates a password.
4. Syscalls call `cap_check(proc->caps, CAP_TABLE_SIZE, kind, rights)`
   before operating on resources. Failure returns either `-EPERM` (most
   syscalls) or `-ENOCAP=130` (sys_reboot precedent).
5. No revocation. To reset caps, restart the process.

## Policy file format

    /etc/aegis/caps.d/<binary-basename>
        service NET_SOCKET FB
        admin   POWER
        # comments allowed; unknown cap names emit a kernel WARN

The kernel-side policy table is loaded at boot by `cap_policy_load()`
reading `/etc/aegis/caps.d/` via VFS. New daemons need both the binary in
the rootfs AND a matching policy file — chronos shipped without one for
two phases (silent `chronos: socket failed` every boot) before 1.0.2 caught
it. If you add a new daemon that needs network, write the cap file at the
same time.

## Runtime delegation (limited)

`sys_cap_grant_runtime(363)` lets a process holding `CAP_KIND_CAP_DELEGATE`
hand a capability to another process. `sys_cap_query(362)` lets a process
inspect another's table. Both are used by stsh's `caps`/`sandbox` builtins.
Most production code paths don't touch them.

## C/Rust boundary

All capability logic lives in `kernel/cap/src/lib.rs` (Rust, no_std).
The C interface is `kernel/cap/cap.h`. Callers: `proc.c` (grant) and
`syscall.c` (check). No other C code calls into the capability module.

## Error code

`ENOCAP = 130` — Aegis-specific, returned when a syscall requires a capability
the calling process does not hold.

## Open work

- **No revocation.** A process restart is the only way to drop a granted
  capability. Long-running daemons keep their service-tier caps for life.
- **No on-disk persistence.** sys_sethostname's buffer lives in kernel BSS;
  any other future "set X via syscall" syscalls would need their own
  persistence story (or rely on userspace re-applying state at boot).
- **Token unforgeability.** Today's caps are (kind, rights) pairs in a
  per-process kernel table — guessable indices, but the table is kernel-only
  so user space cannot reach them. If cross-process token passing ever needs
  to survive untrusted hops (e.g. across an AF_UNIX socket without
  SCM_RIGHTS-equivalent kernel mediation), tokens should grow a kernel-
  generated random `id` field. Not currently a load-bearing concern.
