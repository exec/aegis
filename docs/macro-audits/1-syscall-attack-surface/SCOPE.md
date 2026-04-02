# Audit 1: Syscall Attack Surface

## Priority: CRITICAL

Every syscall is a direct ring-0 entry point from untrusted userspace.
This is the #1 source of kernel CVEs in any OS.

## Files to review

| File | LOC | Focus |
|------|-----|-------|
| `kernel/syscall/sys_process.c` | 732 | fork, clone, waitpid, exit |
| `kernel/syscall/sys_exec.c` | 986 | execve, spawn |
| `kernel/syscall/sys_identity.c` | 297 | getpid, setuid, setsid, uname, reboot, getuid/gid |
| `kernel/syscall/sys_cap.c` | 120 | cap_grant_exec, cap_grant_runtime, cap_query |
| `kernel/syscall/sys_file.c` | 1150 | open, read, write, close, stat, lseek, dup, ioctl, getdents |
| `kernel/syscall/sys_time.c` | 95 | nanosleep, clock_gettime, clock_settime |
| `kernel/syscall/sys_socket.c` | 1003 | socket, bind, listen, accept, connect, send, recv, epoll |
| `kernel/syscall/sys_memory.c` | 438 | mmap, munmap, mprotect, brk — virtual memory manipulation |
| `kernel/syscall/sys_signal.c` | 293 | sigaction, sigprocmask, sigreturn, kill |
| `kernel/syscall/sys_io.c` | 223 | writev, netcfg, fb_map, spawn, reboot |
| `kernel/syscall/sys_disk.c` | 222 | blkdev_io, blkdev_list, gpt_rescan |
| `kernel/syscall/syscall.c` | 201 | dispatch table, syscall entry validation |
| `kernel/syscall/futex.c` | 79 | futex wait/wake |
| `kernel/syscall/sys_random.c` | 46 | getrandom |
| `kernel/arch/x86_64/syscall_entry.asm` | — | register save/restore, SYSRET path |

## Threat model

- Attacker controls all 6 syscall arguments (rdi, rsi, rdx, r10, r8, r9)
- Attacker controls all user memory contents
- Attacker can race syscalls from multiple threads (CLONE_VM)
- Attacker can signal themselves during a syscall

## Checklist — what each agent must verify

### Input validation
- [ ] Every user pointer passed to copy_from_user/copy_to_user before dereference
- [ ] No raw user pointer dereference without STAC/CLAC
- [ ] Integer overflow on (ptr + len) checked before use
- [ ] Negative lengths rejected (signed/unsigned confusion)
- [ ] File descriptors bounds-checked against FD_MAX
- [ ] Path strings null-terminated with bounded copy (no unbounded strcpy)

### TOCTOU (time-of-check-to-time-of-use)
- [ ] No double-fetch from userspace (read value once, use the copy)
- [ ] Capability checks and resource access are atomic (no window between check and use)
- [ ] Path resolution doesn't race with symlink creation

### Resource leaks on error paths
- [ ] Every allocation freed on every error return
- [ ] Partial state rolled back on failure (e.g., half-created process in fork)
- [ ] File descriptors not leaked on exec failure

### Privilege escalation
- [ ] Capability check before every privileged operation
- [ ] No capability bypass via fd inheritance across exec
- [ ] sigreturn cannot forge arbitrary RIP/RFLAGS/CS/SS
- [ ] SYSRET non-canonical RIP check present

### Concurrency (with threads)
- [ ] fd_table operations protected when CLONE_FILES is set
- [ ] mmap state protected when CLONE_VM is set
- [ ] Signal delivery during syscall doesn't corrupt state

## Output format

```
### [CRITICAL|HIGH|MEDIUM|LOW] — Short title

**File:** `path/to/file.c:123`
**Bug class:** [overflow|TOCTOU|use-after-free|missing-check|leak|...]
**Description:** What's wrong and how to trigger it.
**Impact:** What an attacker gains.
**Fix:** Specific code change needed.
```
