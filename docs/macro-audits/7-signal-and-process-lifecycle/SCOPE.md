# Audit 7: Signal & Process Lifecycle

## Priority: HIGH

Known open bug: C5 (SYSRET signal delivery corrupts callee-saved registers).
fork/exec/exit have complex interactions with signals, file descriptors,
capabilities, and memory maps.

## Files to review

| File | LOC | Focus |
|------|-----|-------|
| `kernel/syscall/sys_process.c` | 732 | fork, clone, waitpid, exit, exit_group |
| `kernel/syscall/sys_exec.c` | 986 | execve, spawn |
| `kernel/syscall/sys_signal.c` | 293 | sigaction, sigprocmask, sigreturn, kill |
| `kernel/sched/signal.c` | — | Signal delivery, frame setup |
| `kernel/sched/sched.c` | 572 | Task state transitions, exit cleanup |
| `kernel/arch/x86_64/syscall_entry.asm` | — | SYSRET path, register save/restore |

## Checklist

### Signal safety
- [ ] C5: SYSRET path saves ALL registers before signal delivery (rbx, rbp, r12-r15)
- [ ] sigreturn validates RIP is canonical (non-canonical → SIGSEGV, not SYSRET #GP)
- [ ] sigreturn sanitizes RFLAGS (no IF/IOPL/VM manipulation)
- [ ] Signal handler address validated (must be in user space)
- [ ] Nested signals don't overflow kernel stack
- [ ] SA_RESTART correctly retries interrupted syscalls

### Process lifecycle
- [ ] fork copies all process state completely (fd_table, VMAs, caps, TLS)
- [ ] fork failure cleans up all partially-copied state
- [ ] execve point-of-no-return: no failure after old state is freed
- [ ] exit_group terminates all threads in the group
- [ ] Zombie reaping via waitpid doesn't leak TCBs
- [ ] Orphan processes reparented to init (pid 1)
- [ ] PID reuse doesn't cause stale references

### File descriptor handling
- [ ] O_CLOEXEC fds closed on exec
- [ ] dup'd fds have correct refcount
- [ ] Pipe fds closed on both ends during exit
- [ ] fork increments fd_table refcount for CLONE_FILES

## Output format

Same as Audit 1.
