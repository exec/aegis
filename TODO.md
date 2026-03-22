# Aegis — Phase Roadmap (Phase 16+)

Current state: interactive musl shell, fork/execve/waitpid, initrd VFS,
capability stub, anonymous mmap, no signals, no pipes, no writable storage.

**End goal:** Aegis boots on real hardware, runs musl userland programs
without modification, and enforces capability-based access control with
no ambient authority — no process can open a file, send a packet, or
spawn a child without an explicit capability token.

Phases are ordered by dependency and user-visible impact.
Each phase delivers something that visibly works by the end.

---

## Phase 16 — Pipes + I/O Redirection

**Unlocks:** shell pipelines (`ls | grep foo`), `>`, `<`, `>>`, `2>&1`

**What to build:**
- `sys_pipe2` (syscall 293): allocate a kernel ring buffer, return two fds
- `sys_dup` / `sys_dup2` (syscall 32/33): duplicate file descriptors
- Pipe VFS ops: `pipe_read_fn`, `pipe_write_fn`, blocking on empty/full
- Shell parser: recognize `|`, `>`, `<`, `>>`, `2>&1`

**Shell upgrades needed:**
- `parse_redirects()`: scan argv for `>file`, `<file`, `>>file`
- Before execve in child: `dup2(pipe_write, STDOUT_FILENO)`, then execve
- Multi-stage pipeline: chain N children with N-1 pipes

**Key constraint:** pipe blocking requires the scheduler's `sched_block` +
`sched_wake` path; the writer blocks when the ring is full, reader blocks
when empty. The VFS write/read ops must call into the scheduler.

**Milestone:** `ls /bin | cat` and `echo hello > /tmp/test` work in the shell.

---

## Phase 17 — Signals

**Unlocks:** Ctrl-C kills foreground process, `kill`, `SIGCHLD` delivery,
`sigaction`, `sigprocmask` — basically all process control.

**What to build:**
- `aegis_process_t` gets: `pending_signals` (bitmask), `signal_mask`,
  `sigaction` table (64 entries), `signal_stack_va`
- `sys_sigaction` (syscall 13), `sys_sigprocmask` (syscall 14),
  `sys_kill` (syscall 62), `sys_getpid`/`sys_getppid` (syscall 39/110)
- Signal delivery: check pending on every return to user space (syscall
  exit + iretq path in isr_common_stub)
- Signal frame: build a `ucontext_t`-shaped frame on the user stack,
  jump to the handler, `sys_sigreturn` (syscall 15) restores context
- Keyboard driver: scancode for Ctrl-C emits `kill(foreground_pgid, SIGINT)`
- Shell: `setpgrp`, track foreground process group, `SIGCHLD` triggers reap

**Key constraint:** signal delivery happens in `isr_post_dispatch` and
`syscall_entry` return paths. The frame must be exact — musl's sigreturn
restores the full `ucontext_t` we pushed.

**Milestone:** Ctrl-C kills a running `cat` without crashing the shell.

---

## Phase 18 — syscall completeness + stat

**Unlocks:** nearly every musl-linked program (`grep`, `sed`, `wc`, `sort`)

**What to build:**
- `sys_stat` / `sys_fstat` / `sys_lstat` (syscalls 4/5/6): populate a
  `struct stat` from VFS metadata; initrd files get fake inode numbers
- `sys_access` (syscall 21): stub returning 0 for initrd files
- `sys_fcntl` (syscall 72): at minimum `F_GETFL`, `F_SETFL` (O_NONBLOCK),
  `F_DUPFD`; return -EINVAL for others
- `sys_ioctl` (syscall 16): `TIOCGWINSZ` returns 80×25; others -EINVAL
- `sys_nanosleep` (syscall 35): block for N ticks via PIT counter
- `sys_getuid/geteuid/getgid/getegid` (32/107/104/108): always return 0

**Key constraint:** musl uses `stat` in `fopen`, `opendir`, and many places.
Without `sys_stat` returning plausible data, most programs segfault in musl
init. The `st_mode` and `st_size` fields are the critical ones.

**Milestone:** `grep`, `wc`, and `sort` compile against musl and run on
initrd files without error.

---

## Phase 19 — Writable Filesystem (virtio-blk + ext2)

**Unlocks:** persistent storage, file creation, build artifacts, anything
that writes files

**What to build:**
- virtio-blk PCI driver: scan PCI bus for vendor 0x1AF4 device 0x1001;
  map BAR0; implement `blk_read` / `blk_write` (512-byte sectors)
- ext2 driver: superblock parse, inode read, directory lookup, file read;
  start read-only, add write in a second pass
- VFS layer extension: `vfs_ops_t` gets `create`, `unlink`, `mkdir`,
  `rename`, `truncate`
- `sys_open` flag handling: `O_CREAT`, `O_WRONLY`, `O_APPEND`, `O_TRUNC`
- `sys_unlink` (syscall 87), `sys_mkdir` (syscall 83), `sys_rename` (82)
- QEMU disk image: `make disk` target creates `build/disk.img` (32 MB
  ext2), `make run` adds `-drive file=build/disk.img,format=raw`

**Key constraint:** ext2 write requires journaling awareness — without a
journal, crashes corrupt the fs. For Phase 19, mount ext2 without journal
(`-O ^has_journal` when formatting) and add fsck to the boot sequence.
Full journaling (ext3) is deferred.

**Milestone:** `echo hello > /mnt/disk/hello.txt && cat /mnt/disk/hello.txt`
persists across reboots.

---

## Phase 20 — Capability System (Aegis Differentiator)

**Unlocks:** the core security model — no process holds ambient authority

**What to build:**
- `kernel/cap/` Rust implementation: `CapToken` (128-bit unforgeable),
  `CapSpace` per process, `cap_derive` (create child cap from parent),
  `cap_revoke` (invalidate subtree)
- Capability kinds: `CAP_FILE(path_prefix, rights)`, `CAP_NET(addr_range)`,
  `CAP_PROC(spawn_limit)`, `CAP_MEM(va_range, flags)`
- `sys_cap_grant` (new): parent grants a derived capability to child fd
- `sys_cap_drop` (new): irrevocably drop a capability from this process
- All resource syscalls (`open`, `connect`, `fork`) check cap before action
- `proc_spawn_init`: grant minimal set — console write + initrd read only
  (no ambient access to disk, no network unless explicitly granted)
- Shell: `cap` builtin to inspect current cap set

**Key constraint:** the Rust `no_std` boundary must remain clean. C callers
use `cap_check_ffi(caps_ptr, kind, rights)` — no Rust types leak into C.
The token must be unforgeable: use a per-boot random secret xor'd into
the handle, validated on every syscall.

**Milestone:** `cat /mnt/disk/secret.txt` fails with ENOCAP from a process
that was not granted filesystem access, even as uid=0.

---

## Phase 21 — Copy-on-Write Fork + Demand Paging

**Unlocks:** fast fork (shell pipelines don't copy gigabytes), stack growth,
efficient memory use for large programs

**What to build:**
- CoW fork: instead of copying all pages, mark both parent and child pages
  read-only; on write fault (#PF error=0x3), allocate a new page, copy,
  remap writable in the faulting process
- Page fault handler: wire `#PF` (vector 14) in `isr_dispatch` to a C
  handler `vm_fault(cr2, error_code)` that handles CoW and demand pages
- Demand stack growth: map a guard page below the stack; on fault, extend
  stack mapping downward (up to a configurable limit)
- `sys_madvise` (syscall 28): `MADV_DONTNEED` — unmap pages (free physical)
- `sys_munmap` with VA reclaim: replace the bump allocator with a freelist
  of VA extents

**Key constraint:** CoW requires the page fault handler to run with
interrupts disabled and cannot allocate memory (PMM alloc from fault
context requires careful re-entrancy analysis). Keep the fault handler
allocation-free — use a pre-reserved emergency page pool.

**Milestone:** `fork` + `execve` in the shell takes <1ms instead of copying
all pages. Verified with `time echo foo`.

---

## Phase 22 — Network Stack

**Unlocks:** servers, curl, DNS, anything networked

**What to build:**
- virtio-net PCI driver: scan for vendor 0x1AF4 device 0x1000; init
  virtqueues; implement DMA-based packet send/receive
- Minimal TCP/IP: integrate lwIP (`no_sys` mode) or build a tiny stack
  (ARP -> IP -> TCP -> socket API); start with UDP + TCP listen/connect
- BSD socket syscalls: `socket` (41), `bind` (49), `listen` (50),
  `accept` (43), `connect` (42), `send`/`recv` (44/45),
  `setsockopt`/`getsockopt` (54/55)
- `sys_poll` / `sys_select` (7/23): needed for multiplexed I/O
- Network capability: `CAP_NET(addr, port_range)` gates `bind`/`connect`
- DHCP client: minimal implementation to get an IP at boot

**Key constraint:** virtio-net DMA buffers must be physically contiguous.
The PMM currently only allocs 4KB pages; Phase 22 needs a multi-page
contiguous allocator (extend PMM with a buddy allocator for
order-2/order-3 allocations).

**Milestone:** `nc -l 8080` serves a TCP connection from inside Aegis;
`nc host 8080` connects to it; both capability-gated.

---

## Phase 23 — SMP (Multi-Core)

**Unlocks:** real parallelism, proper APIC timer, production-grade kernel

**What to build:**
- ACPI MADT parse: count application processors, extract APIC IDs
- AP bringup: trampoline at 0x8000, SIPI sequence, GDT/IDT on each AP
- Per-CPU data: `cpu_t` struct per core (run queue, current task, TSS,
  GDT, kernel stack)
- Spinlocks: `spinlock_t` with `lock xchg` for scheduler + PMM + VMM
- Per-CPU run queues: `sched_tick` operates on local queue; work-stealing
  for load balancing
- IPI: TLB shootdown on `vmm_unmap`, preemption IPI when waking a task
- APIC timer: replace PIT with per-CPU LAPIC timer; calibrate against PIT

**Key constraint:** single-writer invariants in Phase 1–22 (global
`s_current`, global PMM bitmap) must be replaced with per-CPU structures
guarded by spinlocks. Do not start Phase 23 until the lock audit for all
global kernel state is complete.

**Milestone:** `make test` runs with `-smp 4`; the kernel boots all 4 CPUs
and the scheduler distributes tasks across them.

---

## Non-phase work (ongoing)

These are not phases — they happen incrementally alongside other work:

- **Real hardware boot:** between Phase 19 and 21, attempt booting Aegis
  on a physical x86-64 machine. Serial output + PS/2 should work; virtio
  drivers won't. A milestone, not a phase.

- **`make test` expansion:** add shell test scenarios (pipes, signals, disk
  writes). The current harness only checks boot output.

- **Syscall coverage audit:** run `strace` on target programs under Linux
  to see which syscalls they invoke; add cheap stubs (`sys_sysinfo`,
  `sys_uname`, `sys_times`) alongside phase work.

- **ELF dynamic linking:** a dynamic linker (`ld.so`) requires file-backed
  `mmap`, `mprotect`, and `AT_DYNAMIC` auxv entries. Target: Phase 19+.

---

## Dependency graph

```
Phase 16 (pipes)
Phase 17 (signals)       ← independent of each other, do in any order
Phase 18 (stat/fcntl)
        |
        v
Phase 19 (ext2/disk) ──── Phase 20 (capabilities)
        |                        |
        v                        v
Phase 21 (CoW/demand paging) ◄───┘
        |
        v
Phase 22 (network)
        |
        v
Phase 23 (SMP)
```

---

*Written 2026-03-21. Revise at the start of each phase brainstorm.*
