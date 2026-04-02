# Audit 2: Concurrency & SMP Safety

## Priority: CRITICAL

Phase 38 added SMP (LAPIC+IOAPIC, ~30 spinlocks, per-CPU state). But most
kernel subsystems were written for single-core. APs currently idle, but any
future work enabling multi-core scheduling will hit these bugs immediately.
Even on single-core, ISR-context races are real today.

## Files to review

Agents should `grep -rn 'spin_lock\|spin_unlock\|sched_block\|sched_wake\|cli\|sti' kernel/`
to find all synchronization points, then audit each for correctness.

| File | LOC | Focus |
|------|-----|-------|
| `kernel/sched/sched.c` | 572 | Run queue, context switch, block/wake |
| `kernel/mm/vmm.c` | 942 | vmm_window_lock, page table walks |
| `kernel/net/tcp.c` | 642 | tcp_lock, ISR-driven RX vs syscall TX |
| `kernel/net/unix_socket.c` | 673 | Peer lifetime, ring buffer access |
| `kernel/net/epoll.c` | 239 | Lost-wakeup patterns |
| `kernel/fs/pipe.c` | 263 | Reader/writer synchronization |
| `kernel/tty/pty.c` | 521 | Master/slave blocking |
| `kernel/syscall/sys_socket.c` | 1051 | waiter_task patterns |
| `kernel/syscall/sys_process.c` | 2059 | fork page copy under locks |
| `kernel/arch/x86_64/idt.c` | — | ISR dispatch, nested interrupt handling |

## Checklist

### Lock correctness
- [ ] Every shared mutable is protected by a lock (or documented why not)
- [ ] Lock ordering matches canonical order: `sched_lock > vmm_window_lock > pmm_lock > kva_lock`
- [ ] No lock held across sched_block (deadlock: holder blocks, unlocker needs lock)
- [ ] IRQ-safe locks use spin_lock_irqsave/spin_unlock_irqrestore
- [ ] No spin_lock in ISR context on a lock that syscall context holds without irqsave

### Atomicity
- [ ] sched_wake has appropriate memory barriers (M1 — known TODO for ARM64)
- [ ] mmap freelist access is single-threaded or locked (M2 — known TODO)
- [ ] fd_table operations are safe under CLONE_FILES
- [ ] VMA table access is safe under CLONE_VM

### ISR safety
- [ ] No blocking calls (sched_block, arp_resolve) reachable from ISR
- [ ] No heap/kva allocation from ISR context
- [ ] PIT tick handler doesn't hold locks that conflict with syscall paths
- [ ] Keyboard ISR signal delivery doesn't corrupt task state

### Deadlock patterns
- [ ] No ABBA lock ordering (A locks X then Y, B locks Y then X)
- [ ] tcp_lock + arp_resolve: ISR path returns -1, doesn't block (verify)
- [ ] vmm_window_lock + pmm_lock nesting is always in the same order

## Output format

Same as Audit 1. Tag deadlocks as CRITICAL, missing-lock-on-SMP as HIGH,
single-core-safe-but-SMP-broken as MEDIUM.
