# Aegis Capability Model

## Principle

No process holds ambient authority. A process starts with exactly the capabilities
it is explicitly granted — nothing more. Not even file access, unless granted.

## What a capability is

A capability is a (kind, rights) pair stored in a per-process kernel table. The
kernel creates capabilities; user space cannot forge or modify them. The table is
embedded in the PCB (`aegis_process_t.caps[]`), inaccessible to user space.

## Capability kinds (Phase 11)

| Kind              | Value | Meaning |
|-------------------|-------|---------|
| CAP_KIND_NULL     | 0     | Empty slot |
| CAP_KIND_VFS_OPEN | 1     | Permission to call sys_open |

## Capability rights (Phase 11)

| Right             | Bit | Meaning |
|-------------------|-----|---------|
| CAP_RIGHTS_READ   | 0   | Read access |
| CAP_RIGHTS_WRITE  | 1   | Write access |
| CAP_RIGHTS_EXEC   | 2   | Execute access |

## Cap table layout

8 slots per process, embedded in the PCB. Slot 0 is the first granted capability.
`kind == 0` (CAP_KIND_NULL) means the slot is empty.

## Lifecycle (Phase 11)

1. `proc_spawn` zeros the cap table.
2. `proc_spawn` calls `cap_grant` to populate initial slots.
3. Syscalls call `cap_check` before operating on resources.
4. No revocation or delegation in Phase 11.

## C/Rust boundary

All capability logic lives in `kernel/cap/src/lib.rs` (Rust, no_std).
The C interface is `kernel/cap/cap.h`. Callers: `proc.c` (grant) and
`syscall.c` (check). No other C code calls into the capability module.

## Error code

`ENOCAP = 130` — Aegis-specific, returned when a syscall requires a capability
the calling process does not hold.

## Future: delegation

When multiple user processes exist, a process should be able to delegate a
capability to a child at spawn time via a `sys_cap_grant(child_pid, cap_index)`
syscall. The recipient gets a copy of the slot in its own table. Revocation
requires `sys_cap_revoke(cap_index)` to null the slot. Neither is implemented
in Phase 11.

## Future: unforgeable tokens

Phase 11 capabilities are slot indices — guessable within a process's own table,
but this is acceptable because the table is per-process and kernel-managed. When
cross-process delegation arrives, capability tokens should carry a kernel-generated
random `id` field to prevent a compromised process from guessing another process's
delegated capability values.
