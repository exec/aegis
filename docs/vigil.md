# Vigil — Aegis Init System

Vigil is the PID 1 init system for Aegis. It supervises services defined
in `/etc/vigil/services/<name>/` and handles graceful shutdown.

## Service Declaration

Each service lives in a directory under `/etc/vigil/services/`:

    /etc/vigil/services/<name>/
        run       — command (absolute path or shell-form for /bin/sh -c)
        policy    — respawn | oneshot; optional: max_restarts=N (default 5)
        mode      — text | graphical (optional; gates on kernel cmdline boot=<mode>)
        user      — username to run as (root only for now)

The `caps` file is no longer read. Capabilities are now declared per-binary
in `/etc/aegis/caps.d/<name>` and applied by the kernel at execve time —
see "Capabilities" below.

## IPC

vigictl communicates with vigil via:
1. Write command to `/run/vigil.cmd`
2. Send SIGUSR1 to vigil (PID from `/run/vigil.pid`)

Commands: `status`, `start <svc>`, `stop <svc>`, `restart <svc>`, `shutdown`.

AF_UNIX socket IPC is deferred to Phase 26 (socket API).

## Logging

Vigil logs to stdout with format: `vigil: <message>`

The `vigil:` prefix is intentional — `[VIGIL]` would contaminate the
boot oracle (grep `^[` filter in run_tests.sh keeps only lines starting
with `[`).

## Capabilities

Capability grants are no longer vigil's concern. The Phase 46c cap-policy
redesign moved capability declaration into per-binary policy files read
by the kernel at execve time:

    /etc/aegis/caps.d/<binary-basename>
        service NET_SOCKET FB
        admin   POWER

Two tiers:
- **service** caps are granted unconditionally at every exec of that binary.
- **admin** caps are granted only when `proc->authenticated == 1`, set by
  `sys_auth_session(364)` after libauth verifies a password.

Vigil itself just spawns binaries (absolute paths via execv, shell forms via
`/bin/sh -c`). The kernel reads the matching `/etc/aegis/caps.d/<basename>`
file inside `sys_execve` and grants the listed caps. The retired
`sys_cap_grant_exec(361)` call and the per-process `exec_caps[]` array no
longer exist. Daemons vigil starts directly only ever get baseline +
service-tier caps. Admin-tier caps require an authenticated session — e.g.
spawned from a post-login shell or by Bastion after credential validation.

## Syscalls Added

| Number | Name | Purpose |
|--------|------|---------|
| 162 | sys_sync | Flush ext2 block cache |
| 228 | sys_clock_gettime | POSIX timespec from 100 Hz PIT ticks |
| 364 | sys_auth_session | Mark current process authenticated (admin-tier caps unlocked) |

## Constraints

- Max 16 services (VIGIL_MAX_SERVICES)
- No AF_UNIX direct (vigil/vigictl IPC is `/run/vigil.cmd` + SIGUSR1)
- No cgroup isolation (future)
- No dependency ordering (services start in readdir order)
- CLOCK_REALTIME = CLOCK_MONOTONIC (RTC not implemented; deferred)
- PID 1 exit triggers arch_request_shutdown (QEMU isa-debug-exit)

## Testing

`tests/tests/login_flow_test.rs` exercises the boot → vigil → getty → login
chain on q35+NVMe.
