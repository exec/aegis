# Micro-Audits — Phase 1

15 focused audits across 3 agents. Each audit produces a findings section
with severity (CRITICAL / HIGH / MEDIUM / LOW), affected files, and
recommended fix.

---

## Agent A — Security & Capability Hardening

### A1. Exec baseline over-grant
Every `execve`'d binary receives AUTH, DISK_ADMIN, THREAD_CREATE, FB in the
baseline cap set. Map which binaries actually USE each cap kind. Produce a
table: cap kind → list of actual consumers → recommendation (keep/remove
from baseline, move to capd policy).

### A2. Signal delivery register corruption (C5) — ✅ FIXED (836f480)
syscall_frame_t expanded with rbx/rbp/r12-r15. syscall_entry.asm pushes
them. signal_deliver_sysret saves full callee-saved + rdi/rsi/rdx into
signal frame. sys_rt_sigreturn restores them. Boot oracle PASS.

### A3. TOCTOU in path resolution
`sys_open`, `sys_execve`, `ext2_open_ex` copy the user path string then
resolve it in multiple steps (symlink walk, component lookup). Audit whether
any path component can change between validation and use. Check: is the
512-byte `resolved` buffer in `ext2_open_ex` safe from concurrent modification
by another thread sharing the same address space (CLONE_VM)?

### A4. /etc/shadow accessibility — ✅ FIXED (fc7cdff)
Four-layer fix: path normalization in sys_open, inode-based AUTH check in
vfs_open after ext2 symlink resolution, AUTH check on initrd fallback,
initrd mode 0555→0640. Boot oracle PASS.

### A5. Capability delegation chain integrity
`sys_cap_grant` (363) allows a process holding a cap to grant it to another.
`capd` holds 15/16 cap slots. Audit: (a) can a process grant a cap it
doesn't hold? (b) can a process escalate rights (e.g., grant rwx when it
only has read)? (c) is there a delegation loop risk? (d) what happens when
CAP_TABLE_SIZE (64) slots are exhausted?

---

## Agent B — Stability & Correctness

### B1. SMP data races catalog
The kernel boots APs but only BSP runs user tasks. When AP scheduling is
enabled, these structures have no locks: mmap freelist (`sys_memory.c`),
fd_table (`fd_table.c`), VMA table (`vma.c`), futex waiters array. For each:
identify the exact race scenario, severity if triggered, and minimal fix
(spinlock, atomic, per-CPU).

### B2. Memory leak per fork/exec cycle
Each `fork` + `exec` + `exit` cycle leaks: (a) kva VA from bump allocator,
(b) mmap pages after mprotect(PROT_NONE), (c) pipe_t ring buffer pages
(deferred cleanup), (d) unix socket ring buffers (orphaned on close).
Quantify: how many pages leak per cycle? At what iteration count does the
system OOM? Is the `ls /` OOM (noted since Phase 45) caused by this?

### B3. ext2 corruption resilience
ext2 has no journal. Audit: (a) what happens on power loss during
`ext2_write` mid-block? (b) is the 16-slot LRU block cache write-back or
write-through? (c) does `ext2_sync` flush all dirty blocks? (d) can a
malformed inode in a user-provided ext2 image crash the kernel (e.g.,
i_block pointing outside the device, infinite indirect block chain)?

### B4. PTY/TTY edge cases
16 static PTY pairs. Audit: (a) what happens when all 16 are allocated and
a 17th is requested? (b) does master close deliver EIO to slave readers?
(c) is there a use-after-free if a slave reader is blocked when master
closes? (d) does SIGHUP fire on session leader exit? (e) what happens to
orphaned process groups?

### B5. Pipe and Unix socket resource exhaustion
Pipes: 4KB ring buffer, single waiter per end. Unix sockets: 32 slots,
deferred ring buffer cleanup. Audit: (a) what happens when pipe buffer is
full and writer has no SIGPIPE handler? (b) can a malicious process exhaust
all 32 unix socket slots? (c) is the deferred ring buffer cleanup in
`unix_sock_free` leak-free? (d) what is the maximum kernel memory consumed
by pipes + unix sockets?

---

## Agent C — Performance & Infrastructure

### C1. Fork page copy hot path
`sys_fork` does a full page copy of the parent's address space. On a 1080p
Lumen process this copies ~8MB+ of framebuffer pages. Audit: (a) how many
pages does a typical fork copy? (b) which pages are MMIO and correctly
skipped? (c) what is the actual wall-clock time per fork on QEMU vs bare
metal? (d) is the batch-yield (every 32 pages) working correctly? (e) what
would COW fork save?

### C2. Compositor dirty-rect leak
Lumen freezes after ~60s on bare metal (ThinkPad Zen 2). Hypothesis:
dirty-rect accumulation or memory leak in terminal rendering. Audit: (a)
does `glyph_window_mark_dirty` accumulate rects without bound? (b) does
terminal scroll allocate memory that is never freed? (c) is the backbuffer
composite loop O(n) in dirty rect count? (d) trace the exact path from
keystroke → terminal update → composite → flip.

### C3. ACPI deep dive
The ACPI mode transition (SMI_CMD write) caused a black screen on ThinkPad.
Audit: (a) is the FADT field parsing correct (offsets 46, 48, 52, 56, 64,
68, 88)? (b) is the DSDT `_S5_` bytecode scan robust (what if `_S5_` appears
in a string literal)? (c) does the SCI handler correctly clear PWRBTN_STS
before re-enabling? (d) should the ACPI mode transition be skipped entirely
on UEFI boots (SCI_EN already set)? (e) what does the ThinkPad X13 Zen 2
FADT actually contain?

### C4. Test coverage gaps
List every kernel subsystem and its test status: (a) covered by boot oracle
only, (b) covered by dedicated test_*.py, (c) no automated test at all.
Flag subsystems with zero coverage. Identify the 3 highest-risk untested
paths.

### C5. Scheduler O(n) scan + sleep inefficiency
`sched_tick` walks the entire circular TCB list including blocked tasks.
`sys_nanosleep` uses sleep_deadline but still occupies a run queue slot.
Audit: (a) how many tasks exist in a typical Lumen session? (b) what
fraction are blocked? (c) what is the per-tick overhead? (d) would a
separate RUNNING-only list + blocked wait queues meaningfully improve
throughput? (e) does the 100Hz PIT tick rate cause visible input latency?
