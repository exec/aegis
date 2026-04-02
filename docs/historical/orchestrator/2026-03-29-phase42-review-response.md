# Phase 42 Review Response — Orchestrator Feedback

Response to the orchestrator's review of the stsh (Phase 42) implementation
plan. Each concern investigated against the actual codebase.

---

## 1. sys_cap_query process walk — "wrong and a security issue"

**Concern:** The plan walks the scheduler ring to find processes by PID without
holding a lock. On SMP this is a data race.

**Finding: Partially valid. The plan should use the existing `proc_find_by_pid()` function, not inline a walk.**

The kernel already has `proc_find_by_pid()` in `kernel/proc/proc.c`:

```c
aegis_process_t *
proc_find_by_pid(uint32_t pid)
{
    aegis_task_t *cur = sched_current();
    if (!cur) return NULL;
    if (cur->is_user) {
        aegis_process_t *p = (aegis_process_t *)cur;
        if (p->pid == pid) return p;
    }
    aegis_task_t *t = cur->next;
    while (t != cur) {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pid == pid) return p;
        }
        t = t->next;
    }
    return NULL;
}
```

This function:
- **Does check `is_user`** before casting (the orchestrator's concern about
  reading `aegis_process_t` fields from kernel tasks is already handled)
- **Does not hold `sched_lock`** — same as every other caller (sys_setpgid,
  sys_kill, procfs operations, sys_waitpid)

The lock concern is technically valid for SMP, but it's a pre-existing
architectural issue shared by every PID lookup in the kernel, not specific to
sys_cap_query. All of these callers walk the scheduler ring without
`sched_lock`:

- `sys_kill` (signal delivery by PID)
- `sys_waitpid` (find child)
- `sys_setpgid` (find target)
- `procfs_read_pid` (/proc/[pid])

Syscalls are non-preemptible (interrupts disabled during syscall dispatch on
the calling core), so the walk is safe against concurrent task exit on the
**same core**. On SMP, a task could exit on another core during the walk —
this is the same TOCTOU window that exists for sys_kill et al.

**Action:** sys_cap_query will use `proc_find_by_pid()` (not inline the walk).
The SMP locking gap is documented in the Phase 43a architecture audit as a
known issue affecting all PID-based syscalls, not just sys_cap_query.

---

## 2. test_sandbox_denied — "testing the wrong thing"

**Concern:** The test expects `echo denied_test` to fail without VFS_WRITE,
but if fd 1 is pre-opened and the cap check is at open() time, the write
would succeed.

**Finding: The plan is correct. VFS_WRITE is enforced at write() call time, not open() time.**

From `kernel/syscall/sys_io.c`:

```c
uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Capability gate — must hold VFS_WRITE before touching any fd. */
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;
```

Every `sys_write` call checks the process's capability table before touching
any fd. A sandboxed child spawned with only `VFS_READ` will have fd 1 open
(sys_spawn copies the parent's fd or opens console) but `write(1, ...)` will
return `-ENOCAP` because the process lacks `CAP_KIND_VFS_WRITE`.

The same pattern applies to `sys_writev` (line 82-84), `sys_read` (checks
`CAP_KIND_VFS_READ`), and `sys_open` (checks `CAP_KIND_VFS_OPEN`).

**The test is correct as designed.** `echo denied_test` with only VFS_READ
will fail because echo's write() syscall is gated.

---

## 3. Privileged history in-memory only — "make sure check happens at session start"

**Concern:** If the privileged check is at exit time and the process is killed,
the history file gets written anyway.

**Finding: The plan already handles this correctly.**

From the plan's `history.c`:

```c
void hist_init(int privileged)
{
    s_privileged = privileged;    /* set once at startup */
    ...
    if (privileged) return;       /* never load for privileged sessions */
}

void hist_save(void)
{
    if (s_privileged) return;     /* never persist privileged sessions */
    ...
}
```

And from `main.c`:

```c
hist_init(has_cap_delegate());    /* checked once at startup via sys_cap_query(0) */
```

The `s_privileged` flag is set at init time from the capability check, not
re-checked at exit. `hist_save()` checks the flag, not the capabilities. If
the process is killed (SIGKILL), `hist_save()` never runs at all — the
in-memory history dies with the process. No history touches disk at any point
during a privileged session.

**No changes needed.**

---

## 4. goto fail_child double-free in sys_spawn cap_mask path

**Concern:** Multiple `kva_free_pages(kstack, 4); goto fail_child;` calls
might double-free if `fail_child` also frees kstack.

**Finding: No double-free. The cleanup is correct.**

The `fail_child` label in sys_spawn:

```c
fail_child:
    vmm_free_user_pml4(child->pml4_phys);
    kva_free_pages(child, 1);              /* frees the child PCB, NOT kstack */
    if (!result) result = (uint64_t)-(int64_t)12;
```

`fail_child` frees the **child PCB** (`child`, 1 page) and the **user PML4**.
It does NOT free kstack. The explicit `kva_free_pages(kstack, 4)` before
`goto fail_child` is the only kstack cleanup path.

The cap_mask validation errors in the plan follow the same pattern — free
kstack explicitly, then goto fail_child which frees child+PML4. No
double-free.

**However:** The plan's cap_mask error paths should set a result code before
the goto:

```c
    /* Cap_mask validation failure should return ENOCAP, not ENOMEM */
    kva_free_pages(kstack, 4);
    result = (uint64_t)-(int64_t)ENOCAP;
    goto fail_child;
```

Without setting result, fail_child defaults to ENOMEM (errno 12), which is
the wrong error for a capability check failure. This is a minor bug in the
plan that will be fixed during implementation.

---

## Summary

| Concern | Verdict | Action |
|---------|---------|--------|
| sys_cap_query walk | Partially valid | Use existing `proc_find_by_pid()`, note SMP gap for Phase 43a audit |
| test_sandbox_denied | Not an issue | VFS_WRITE gated at write() time, test is correct |
| History privileged check | Already correct | No changes needed |
| fail_child double-free | Not an issue | Set correct errno before goto (minor fix) |

The orchestrator's review caught one real issue (use proc_find_by_pid instead
of inlining a walk) and one plan detail (missing errno in cap_mask error path).
The other two concerns were investigated and found to be already handled
correctly by the existing kernel architecture and plan code.
