# Macro Audits — Deep Multi-Agent Code Reviews

These are large-scale audits designed to be run with parallel subagents.
Each audit category has its own directory with a `SCOPE.md` defining what
to review, what to look for, and how to structure findings.

Audits are ordered by **risk priority** — highest-impact areas first.

## Audit Categories (descending priority)

| # | Category | Why it's high priority | Key files | Est. agents |
|---|----------|----------------------|-----------|-------------|
| 0 | **Structural review** | ✅ DONE. Directory moves + file splits complete. | — | — |
| 1 | **Syscall attack surface** | Every syscall is a ring-0 entry point. Integer overflows, missing validation, TOCTOU races, unchecked user pointers. This is where CVEs live. | `kernel/syscall/*.c` (5.8K LOC) | 3-4 |
| 2 | **Concurrency & SMP** | 505 spinlock call sites. Phase 38 added SMP but most subsystems were written single-core. Lock ordering violations, missing locks on shared state, IRQ-context deadlocks. | All files with `spin_lock`, `sched_block`, ISR handlers | 3-4 |
| 3 | **Memory safety** | VMM (942 LOC), PMM, KVA allocator, mmap freelist. Use-after-free on page table teardown, double-free, OOM paths that panic instead of returning errors. The `ls /` OOM bug lives here. | `kernel/mm/*.c`, `kernel/syscall/sys_memory.c` | 2-3 |
| 4 | **Network stack** | TCP state machine (642 LOC), ARP, IP, UDP, unix sockets (673 LOC). Outbound TCP is broken. Shared static TX buffers. ISR-context packet processing races. | `kernel/net/*.c` (3.2K LOC) | 2-3 |
| 5 | **Filesystem integrity** | ext2 (1177 LOC) is read-write with no journal. Crash = corruption. Symlink traversal, path canonicalization, permission bypass. VFS merge semantics (initrd + ext2 + ramfs). | `kernel/fs/*.c` (5.5K LOC) | 2-3 |
| 6 | **Capability model** | Only 110 LOC of Rust — but the *enforcement points* are scattered across every syscall. Missing cap checks, over-broad baseline grants, cap delegation without revocation. | `kernel/cap/`, all `cap_check` call sites | 2 |
| 7 | **Signal & process lifecycle** | C5 FIXED. Fork/exec/exit race conditions. Orphan handling. PID reuse. | `sys_signal.c`, `sys_process.c` (~730), `sys_exec.c` (~986), `signal.c` | 2 |
| 8 | **Userspace hardening** | pwn.c (1.3K LOC) exists but is it comprehensive? Stack canaries, ASLR (absent), W^X enforcement, fd leak across exec, /etc/shadow access. | `user/*.c`, capability grants in vigil/capd | 2 |
| 9 | **Driver robustness** | xHCI (996 LOC), NVMe (501 LOC), virtio-net (505 LOC). Timeout handling, malformed device responses, resource cleanup on error paths. | `kernel/drivers/*.c` | 2 |
| 10 | **Build & toolchain** | Makefile dependency tracking (no `-MMD`), stale binary problem, `make clean` gaps, ARM64 build broken. | `Makefile`, `tools/`, `kernel/arch/arm64/` | 1 |

## How to run an audit

Each category directory contains a `SCOPE.md` with:
- **Files to review** — exact paths and line counts
- **Threat model** — what an attacker can do, what bugs look like
- **Checklist** — specific things each agent should verify
- **Output format** — how to report findings (severity, file:line, description, fix)

To run: dispatch N parallel Opus subagents, one per file or file group within
the category. Each agent reads `SCOPE.md` + assigned files, reports findings
to a `findings.md` in the category directory.

## Relationship to micro-audits

`docs/micro-audits/` = small focused reviews (single function, single bug class).
`docs/macro-audits/` = systematic multi-file reviews with parallel agents.

Micro-audits are for squeezing in quick wins. Macro-audits are for thorough
coverage of an entire subsystem.
