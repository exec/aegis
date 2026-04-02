# Audit 6: Capability Model Enforcement

**Date:** 2026-04-01 | **Auditor:** Agent (Opus)

## Cap Kind → Enforcement Map

| Cap Kind | # | Enforcement Points | Status |
|----------|---|-------------------|--------|
| VFS_OPEN (1) | sys_open | OK |
| VFS_WRITE (2) | sys_write, writev, mkdir, unlink, rename, symlink, chmod, fchmod | OK |
| VFS_READ (3) | sys_read, readlink | OK |
| AUTH (4) | sys_open (path), vfs_open (ext2 inode), vfs_open (initrd) | OK (3-layer defense) |
| CAP_GRANT (5) | cap_grant_exec, cap_grant_runtime | OK |
| SETUID (6) | setuid, setgid, chown, fchown, lchown | OK |
| NET_SOCKET (7) | sys_socket (AF_INET) | **ISSUE: sys_socketpair missing** |
| NET_ADMIN (8) | sys_netcfg, clock_settime | OK |
| THREAD_CREATE (9) | sys_clone (CLONE_VM) | OK |
| PROC_READ (10) | sys_kill, sys_setfg | **ISSUE: TIOCSPGRP bypasses** |
| DISK_ADMIN (11) | blkdev_list, blkdev_io, gpt_rescan | OK |
| FB (12) | sys_fb_map | **ISSUE: over-granted in baseline** |
| CAP_DELEGATE (13) | cap_grant_runtime, sys_spawn cap_mask | OK |
| CAP_QUERY (14) | sys_cap_query (other-pid) | OK |
| IPC (15) | sys_socket (AF_UNIX), memfd_create | **ISSUE: sys_socketpair missing** |
| POWER (16) | sys_reboot | OK |

## Findings

### HIGH — sys_socketpair bypasses all capability checks
**File:** sys_socket.c:669
**Description:** Creates two connected sockets without any cap_check. sys_socket checks NET_SOCKET/IPC but socketpair does not. Sandboxed process can create socket pairs.
**Fix:** Add cap_check for CAP_KIND_IPC at top of sys_socketpair.

### MEDIUM — Exec baseline over-grants CAP_KIND_FB
**File:** sys_process.c:970
**Description:** Every exec'd binary can map the physical framebuffer. Only Bastion/Lumen need this. Any process can screen-scrape passwords.
**Fix:** Remove FB from execve baseline. Grant via capd/exec_caps to compositor only.

### MEDIUM — Exec baseline over-grants PROC_READ with WRITE rights
**File:** sys_process.c:969
**Description:** Every binary gets PROC_READ+WRITE. WRITE gates sys_kill — every process can kill any other.
**Fix:** Baseline should be PROC_READ only. WRITE via exec_caps to shells/vigil.

### MEDIUM — TIOCSPGRP bypasses PROC_READ cap gate
**File:** sys_file.c:453
**Description:** sys_ioctl(TIOCSPGRP) changes TTY fg_pgrp without cap_check. sys_setfg does the same op but IS gated.
**Fix:** Add cap_check for PROC_READ+WRITE in TIOCSPGRP case.

### MEDIUM — sys_pipe2 has no capability check
**File:** sys_file.c:656
**Description:** Pipes allocated without cap check. Inconsistent with AF_UNIX requiring IPC cap.
**Fix:** Gate behind CAP_KIND_IPC or document as intentional.

### LOW — sys_sync has no capability check (DoS via I/O storm)
### LOW — SCM_RIGHTS implicitly delegates capabilities via fd passing

## Positive Findings

- CAP_TABLE_SIZE consistent between C (cap.h) and Rust (lib.rs) — both 64
- cap_check bounds-checks via Rust clamping — no OOB possible
- /etc/shadow has 3-layer defense-in-depth (path, ext2 inode, initrd path)
- cap_grant_exec and cap_grant_runtime both verify caller holds the delegated cap (fixed earlier this session)
- CAP_KIND_POWER correctly excluded from baseline
- Init (PID 1) correctly receives all 15 caps for delegation to services
