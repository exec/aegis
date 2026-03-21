# Phase 11: Capability System

## Goal

Replace the Phase 1 capability stub with a real, minimal capability system:

1. **`CapSlot` type + `cap_grant`/`cap_check` FFI** — Rust-implemented, C-callable.
2. **Per-process cap table** — 8 `cap_slot_t` entries embedded in `aegis_process_t`,
   parallel to the fd table.
3. **Initial grant at spawn** — `proc_spawn` grants `CAP_KIND_VFS_OPEN | CAP_RIGHTS_READ`
   to every user process; emits `[CAP] OK: 1 capability granted to init`.
4. **`sys_open` gate** — first syscall to require a capability; returns `-ENOCAP` if the
   process does not hold `CAP_KIND_VFS_OPEN | CAP_RIGHTS_READ`.
5. **`docs/capability-model.md`** — the architectural reference document referenced in
   CLAUDE.md ("read this before touching anything in cap/").

No delegation, no revocation, no user-visible capability handles in Phase 11. The
per-process table is fully kernel-managed: the kernel grants at spawn and checks at
syscall entry. User space cannot manipulate capability slots directly.

---

## Background

### Why now

Phases 1–10 built the infrastructure the capability system depends on: memory
management, process model, per-process state (PCB with fd table), and syscall
dispatch with user-pointer validation. CLAUDE.md explicitly deferred capability
implementation until these were solid. They are solid.

Every syscall added without capability enforcement is wrong by design. `sys_open`
is the first syscall that grants access to a resource (a file). Making it the first
capability-gated syscall is architecturally correct: all file I/O flows through
`sys_open`, so a single gate covers `sys_read`, `sys_close`, and any future file
syscalls.

### The C/Rust boundary

CLAUDE.md mandates that the capability subsystem lives in Rust (`no_std`). The
boundary is a stable C header (`cap.h`) exposing `cap_slot_t` and two C-callable
functions. Nothing outside `kernel/cap/` may implement capability logic. The
boundary is crossed at syscall entry (`cap_check`) and process spawn (`cap_grant`).

### Why `ENOCAP` not `EPERM`

`EPERM` means "operation not permitted by the OS policy." `ENOCAP` means "you do
not hold the capability token required for this operation." These are distinct
failure modes. Aegis uses `ENOCAP = 130` — outside the range of standard Linux
errnos used in this codebase (2, 9, 14, 24) and distinct from ENOSYS (-1).

---

## Architecture

### Rust module: `kernel/cap/src/lib.rs`

Replace the Phase 1 stub with real types and functions.

**`CapSlot`** — `#[repr(C)]` so C can embed it directly in the PCB struct:

```rust
#[repr(C)]
pub struct CapSlot {
    pub kind:   u32,   /* CAP_KIND_* — 0 means empty */
    pub rights: u32,   /* CAP_RIGHTS_* bitfield */
}
```

**`cap_init`** — updated message:

```rust
#[no_mangle]
pub extern "C" fn cap_init() {
    // SAFETY: serial_init() is called before cap_init() in kernel_main.
    // The pointer is a valid NUL-terminated C string in read-only data.
    unsafe {
        serial_write_string(
            c"[CAP] OK: capability subsystem initialized\n".as_ptr() as *const u8
        );
    }
}
```

**`cap_grant`** — write a `(kind, rights)` pair into the first empty slot:

```rust
#[no_mangle]
pub extern "C" fn cap_grant(
    table: *mut CapSlot,
    n: u32,
    kind: u32,
    rights: u32,
) -> i32 {
    // SAFETY: `table` points to `n` CapSlot entries in the caller's PCB.
    // Called only from proc_spawn with proc->caps and CAP_TABLE_SIZE.
    // The PCB lives for the duration of the process; no concurrent mutation
    // occurs because proc_spawn runs before the task is added to the run queue.
    let slots = unsafe { core::slice::from_raw_parts_mut(table, n as usize) };
    for (i, slot) in slots.iter_mut().enumerate() {
        if slot.kind == 0 {
            slot.kind   = kind;
            slot.rights = rights;
            return i as i32;
        }
    }
    -(ENOCAP as i32)
}
```

**`cap_check`** — return 0 if any slot matches `kind` with at least `rights`:

```rust
#[no_mangle]
pub extern "C" fn cap_check(
    table: *const CapSlot,
    n: u32,
    kind: u32,
    rights: u32,
) -> i32 {
    // SAFETY: `table` points to `n` CapSlot entries in the caller's PCB.
    // Called from syscall handlers with proc->caps and CAP_TABLE_SIZE.
    // The PCB is valid for the lifetime of the process; syscalls run on the
    // process's kernel stack with the process's PCB pointer from s_current.
    let slots = unsafe { core::slice::from_raw_parts(table, n as usize) };
    for slot in slots {
        if slot.kind == kind && (slot.rights & rights) == rights {
            return 0;
        }
    }
    -(ENOCAP as i32)
}

const ENOCAP: u32 = 130;
```

### Updated `kernel/cap/cap.h`

```c
#ifndef CAP_H
#define CAP_H

#include <stdint.h>

/* cap_slot_t — one entry in a per-process capability table.
 * kind == CAP_KIND_NULL means the slot is empty.
 * Laid out as #[repr(C)] in Rust; size = 8 bytes. */
typedef struct {
    uint32_t kind;    /* CAP_KIND_* */
    uint32_t rights;  /* CAP_RIGHTS_* bitfield */
} cap_slot_t;

#define CAP_TABLE_SIZE    8u

/* Capability kinds */
#define CAP_KIND_NULL     0u   /* empty slot */
#define CAP_KIND_VFS_OPEN 1u   /* permission to call sys_open */

/* Capability rights (bitfield) */
#define CAP_RIGHTS_READ   (1u << 0)
#define CAP_RIGHTS_WRITE  (1u << 1)
#define CAP_RIGHTS_EXEC   (1u << 2)

/* ENOCAP — Aegis-specific error: no matching capability found.
 * Value 130 is outside the range of Linux errnos used in this kernel. */
#define ENOCAP 130u

/* cap_init — initialize the capability subsystem.
 * Prints [CAP] OK line. Called from kernel_main before sched_init. */
void cap_init(void);

/* cap_grant — write (kind, rights) into the first empty slot of table[0..n).
 * Returns the slot index on success.
 * Returns -ENOCAP if all slots are occupied. */
int cap_grant(cap_slot_t *table, uint32_t n, uint32_t kind, uint32_t rights);

/* cap_check — return 0 if table[0..n) contains a slot with matching kind
 * and at least the requested rights; return -ENOCAP otherwise. */
int cap_check(const cap_slot_t *table, uint32_t n, uint32_t kind, uint32_t rights);

#endif /* CAP_H */
```

### Changes to `kernel/proc/proc.h`

Add `#include "cap.h"` and a `caps[]` field to `aegis_process_t`:

```c
#include "cap.h"

typedef struct {
    aegis_task_t  task;                    /* MUST be first */
    uint64_t      pml4_phys;
    vfs_file_t    fds[PROC_MAX_FDS];       /* Phase 10 */
    cap_slot_t    caps[CAP_TABLE_SIZE];    /* Phase 11 — capability table */
} aegis_process_t;
```

Size impact: `CAP_TABLE_SIZE` × 8 bytes = 64 bytes added to the PCB. Total PCB
size remains well under 4 KB (the KVA page allocated for it).

### Changes to `kernel/proc/proc.c`

Add `#include "cap.h"` (already pulled in transitively via `proc.h`, but explicit
is better). In `proc_spawn`, after zeroing the fd table:

```c
/* Zero cap table — all slots start empty. */
uint32_t ci;
for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
    proc->caps[ci].kind   = CAP_KIND_NULL;
    proc->caps[ci].rights = 0;
}

/* Grant initial capabilities to this user process. */
cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_OPEN, CAP_RIGHTS_READ);
printk("[CAP] OK: 1 capability granted to init\n");
```

**Placement in `kernel_main` output sequence:** `proc_spawn_init()` is called
after `sched_spawn(task_idle)` and before `vmm_teardown_identity()`. The
`[CAP] OK: 1 capability granted to init` line therefore appears between
`[INITRD] OK: 1 file registered` and `[VMM] OK: identity map removed`.

### Changes to `kernel/syscall/syscall.c`

`syscall.c` already includes `proc.h` (Phase 10). `proc.h` now includes `cap.h`,
so `ENOCAP`, `cap_check`, `CAP_KIND_VFS_OPEN`, and `CAP_RIGHTS_READ` are all
available without an additional include.

Add capability check at the top of `sys_open`, before path validation:

```c
static uint64_t
sys_open(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_OPEN, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    /* ... existing: path copy, vfs_open, fd allocation ... */
}
```

The cast chain `(uint64_t)-(int64_t)ENOCAP` produces the two's-complement
negative value `-130` as a `uint64_t`, consistent with how other error codes are
returned (`(uint64_t)-14` for EFAULT, etc.).

No other syscalls change. `sys_read` and `sys_close` are implicitly gated: a
process can only hold a valid fd if `sys_open` succeeded, which requires the
capability.

### New file: `docs/capability-model.md`

The architectural reference document CLAUDE.md points to. Content:

```markdown
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
```

### Updated `tests/expected/boot.txt`

Two changes from Phase 10:

1. `[CAP] OK: capability subsystem reserved` → `[CAP] OK: capability subsystem initialized`
2. New line `[CAP] OK: 1 capability granted to init` added between `[INITRD]` and `[VMM] identity map removed`

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[CAP] OK: capability subsystem initialized
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SMAP] OK: supervisor access prevention active
[VFS] OK: initialized
[INITRD] OK: 1 file registered
[CAP] OK: 1 capability granted to init
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 3 tasks
[MOTD] Hello from initrd!
[AEGIS] System halted.
```

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/cap/src/lib.rs` | Add `CapSlot`, `cap_grant`, `cap_check`; update `cap_init` message |
| `kernel/cap/cap.h` | Add `cap_slot_t`, constants (`CAP_KIND_*`, `CAP_RIGHTS_*`, `ENOCAP`), declare `cap_grant`/`cap_check` |
| `kernel/proc/proc.h` | Add `#include "cap.h"`; add `caps[CAP_TABLE_SIZE]` to `aegis_process_t` |
| `kernel/proc/proc.c` | Zero cap table + `cap_grant` + `printk` in `proc_spawn` |
| `kernel/syscall/syscall.c` | Add `cap_check` at top of `sys_open` |
| `tests/expected/boot.txt` | Update "reserved"→"initialized"; add grant line |
| `docs/capability-model.md` | New — architectural reference |
| `.claude/CLAUDE.md` | Update build status table |

No Makefile changes: `kernel/cap/` already builds as `libcap.a` and links into
the kernel ELF. No new source files in the C tree.

---

## Test Oracle

`make test` exits 0. Three audits:

```bash
# cap_check called in sys_open
grep -n 'cap_check' kernel/syscall/syscall.c

# ENOCAP defined only in cap.h
grep -rn 'ENOCAP' kernel/

# cap table in aegis_process_t
grep -n 'caps\[' kernel/proc/proc.h
```

---

## Success Criteria

1. `make test` exits 0 with updated oracle.
2. `grep -n 'cap_check' kernel/syscall/syscall.c` shows the check in `sys_open`.
3. `grep -rn 'ENOCAP' kernel/` shows definition only in `cap.h`, use only in
   `lib.rs` and `syscall.c`.
4. Rust crate compiles without warnings under `cargo +nightly build --release`.

---

## Phase 12 Forward-Looking Constraints

**Capability delegation deferred.** No `sys_cap_grant` or `sys_cap_revoke` in
Phase 11. When multiple user processes exist, a parent must be able to pass a
capability to a child at spawn time. Phase 12 should add `sys_cap_grant(cap_index)`
to transfer a copy of a capability slot to a newly spawned child process.

**Unforgeable token IDs deferred.** Phase 11 slots have only `(kind, rights)`.
When cross-process delegation arrives, add a `uint64_t id` field to `CapSlot`
populated with a kernel-generated value. This prevents a compromised process from
guessing delegated capability values belonging to other processes.

**`sys_write` capability deferred.** `sys_write(fd=1)` bypasses the fd table and
calls `printk` directly. When a real stdout driver exists (backed by a VFS file
object), `sys_write` should require `CAP_KIND_VFS_WRITE | CAP_RIGHTS_WRITE`.

**`cap_init` serial-only output.** The Rust `cap_init` writes directly to serial
(not through `printk`) because no `printk` Rust FFI wrapper exists. When a
`printk_ffi` wrapper is added to `cap.h`, migrate `cap_init` to use it so VGA
output is consistent with all other subsystem init lines.

**Per-CPU safety.** `cap_check` and `cap_grant` access the PCB through a raw
pointer. Single-CPU only; no locking needed. SMP requires either per-CPU PCB
pointers or a spinlock around cap table mutations.
