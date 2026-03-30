# Phase 42: stsh (the Styx Shell) — Design Spec

## Goal

Build stsh (`/bin/stsh`), the capability-aware management shell for Aegis.
stsh is the system's control plane — the only interface through which an
authenticated operator can query and restrict capabilities. It replaces
`/bin/sh` as the default login shell while remaining a fully functional
interactive POSIX-like shell.

## Architecture

stsh is a user-space binary forked from the existing `/bin/sh` codebase.
It adds line editing, history, tab completion, environment variable expansion,
capability builtins (`caps`, `sandbox`), and paste detection. The existing
pipeline/redirect executor is ported intact.

The kernel gains two new capability kinds (`CAP_KIND_CAP_DELEGATE`,
`CAP_KIND_CAP_QUERY`), one new syscall (`sys_cap_query`), and a cap_mask
extension to `sys_spawn` for sandboxed process creation.

## Kernel Work

### New Capability Kinds

Added to `kernel/cap/cap.h` and `kernel/cap/src/lib.rs`:

| Kind | Value | Purpose |
|------|-------|---------|
| `CAP_KIND_CAP_DELEGATE` | 13 | May restrict caps on spawn via cap_mask |
| `CAP_KIND_CAP_QUERY` | 14 | May introspect any process's capability set |

Neither is in the exec baseline. They are granted exclusively through the
login chain via Vigil service config and exec_caps.

### New Syscall: sys_cap_query (362)

```c
int64_t sys_cap_query(uint64_t pid, uint64_t buf_uptr, uint64_t buflen);
```

- `pid == 0`: returns calling process's own caps (always allowed, no cap check)
- `pid != 0`: returns target process's caps (requires `CAP_KIND_CAP_QUERY`)
- Copies up to `buflen` bytes of the target's `caps[]` array to user buffer
- Returns number of slots copied on success
- Errors: `-ENOCAP` (no CAP_QUERY for pid!=0), `-ESRCH` (no such pid),
  `-EFAULT` (bad pointer)

### sys_spawn cap_mask Extension

sys_spawn (514) gains a 5th parameter: `cap_mask_uptr`.

```c
uint64_t sys_spawn(path, argv, envp, stdio_fd, cap_mask_uptr);
```

- If `cap_mask_uptr == NULL` (0): existing behavior (baseline + exec_caps)
- If `cap_mask_uptr != NULL`: pointer to user-space `cap_slot_t[CAP_TABLE_SIZE]`
  - Child gets ONLY the caps listed in cap_mask. Baseline is NOT applied.
    exec_caps are NOT applied.
  - Requires `CAP_KIND_CAP_DELEGATE` in caller's cap table. Returns `-ENOCAP`
    without it.
  - Kernel validates each entry: kind must be `< 16`, rights must be valid bits.
  - Caller can only grant caps that it itself holds. Attempting to grant a cap
    not in the caller's own table returns `-ENOCAP`.

### Login Chain Update

Vigil's getty/login service config (`/etc/vigil/services/getty/caps`) adds:

```
CAP_DELEGATE
CAP_QUERY
```

Login pre-registers both for the shell via
`sys_cap_grant_exec(CAP_KIND_CAP_DELEGATE, CAP_RIGHTS_READ)` and
`sys_cap_grant_exec(CAP_KIND_CAP_QUERY, CAP_RIGHTS_READ)` before execve.
stsh receives them after exec.

## Shell Architecture

### File Structure

| File | Responsibility |
|------|---------------|
| `user/stsh/main.c` | Entry, REPL loop, signal setup, paste detection |
| `user/stsh/editor.c` | Line editing (cursor, insert, delete, home/end) |
| `user/stsh/history.c` | Ring buffer (64 entries), persistent save/load, privileged detection |
| `user/stsh/complete.c` | Tab completion (command + file), directory scanning |
| `user/stsh/parser.c` | Tokenizer, pipe/redirect parsing (ported from existing shell) |
| `user/stsh/exec.c` | Pipeline execution, fork+exec, sandbox spawn, builtin dispatch |
| `user/stsh/caps.c` | `caps` and `sandbox` builtins, sys_cap_query wrapper, cap name table |
| `user/stsh/env.c` | Environment variable storage, $VAR expansion |
| `user/stsh/stsh.h` | Shared types, constants, function prototypes |

### Terminal Mode

stsh runs in raw terminal mode (ICANON and ECHO disabled via termios). Every
keystroke is handled by the editor. This enables arrow keys, home/end, and
in-line editing without relying on the TTY line discipline.

### Prompt

```
user@aegis:path$    (without CAP_DELEGATE)
user@aegis:path#    (with CAP_DELEGATE)
```

- `user`: from USER env var, fallback "aegis"
- `aegis`: hardcoded hostname for v1
- `path`: getcwd(), leading HOME replaced with `~`
- `$` vs `#`: determined at startup by `sys_cap_query(0)`. Cached.
- Rebuilt after every `cd` (path changes).

### Builtins

| Builtin | Description |
|---------|-------------|
| `exit [n]` | Exit with code (default 0) |
| `cd [path]` | Change directory (default HOME or /) |
| `help` | Show builtin list |
| `caps [pid]` | Display capability set. No args = self. With PID = target (requires CAP_QUERY). |
| `sandbox -allow CAP[,CAP,...] -- command [args...]` | Spawn with allowlist-only caps via sys_spawn cap_mask. Requires CAP_DELEGATE. |
| `export VAR=value` | Set environment variable |
| `env` | Print all environment variables |

**caps output format:**
```
VFS_OPEN(r) VFS_READ(r) VFS_WRITE(w) NET_SOCKET(r) CAP_DELEGATE(r) CAP_QUERY(r)
```
Human-readable names, rights in parens (r=read, w=write, x=exec). Empty slots
omitted.

**sandbox syntax:**
```
sandbox -allow VFS_READ,VFS_WRITE -- cat /etc/motd
```
Allowlist-only. No subtractive mode. New cap kinds added in future kernel
versions are denied by default.

**sandbox implementation:** Builds a `cap_slot_t[CAP_TABLE_SIZE]` array from
the allowlist, calls `sys_spawn(path, argv, envp, -1, cap_mask)`. Waits for
child exit. Reports exit code.

### Environment Variables

- In-process array (max 64 entries, `KEY=VALUE` strings)
- Initialized from envp passed by login
- `export VAR=value` adds/updates
- Children inherit via execve/sys_spawn envp
- Expansion: `$VAR`, `${VAR}`, and `$?` (last exit code)
- No command substitution, no arithmetic. Unset vars expand to empty string.

### Line Editing

Raw mode keystrokes:

| Key | Action |
|-----|--------|
| Printable char | Insert at cursor, shift right |
| Backspace/DEL (127) | Delete char before cursor |
| Ctrl-D | EOF (exit if line empty) |
| Left arrow (ESC[D) | Move cursor left |
| Right arrow (ESC[C) | Move cursor right |
| Home (ESC[H) | Move cursor to start |
| End (ESC[F) | Move cursor to end |
| Up arrow (ESC[A) | Previous history entry |
| Down arrow (ESC[B) | Next history entry (or empty line) |
| Tab | Trigger completion |
| Enter | Submit line |
| Ctrl-C | Discard line, new prompt |
| Ctrl-L | Clear screen, redraw prompt + line |

### History

- 64-entry ring buffer, in-memory always
- Up/down arrows navigate. Current line saved before navigating.
- **Privileged sessions** (CAP_DELEGATE held): history is in-memory only.
  Never written to disk. Dies with the session.
- **Non-privileged sessions**: persisted to `~/.stsh_history` on exit, loaded
  on startup.
- Duplicate consecutive entries suppressed.

### Tab Completion

- First token: **command completion**. Scans each directory in PATH, collects
  executable names matching prefix.
- Subsequent tokens: **file completion**. Scans directory (cwd or specified
  path), collects entries matching prefix.
- Single match: complete inline.
- Multiple matches: display all candidates below prompt, redraw.
- Standard file completion — all visible entries regardless of permissions (v1).
  Capability-aware filtering is v2 work.

### Paste Detection

Multi-byte read detection: a single `read()` returning 4+ bytes indicates a
paste event (normal typing and escape sequences are 1-3 bytes at 100Hz PIT).

On paste detection:
1. Buffer all bytes (up to 4096, truncate with warning beyond that)
2. Count newlines
3. Display bordered preview of pasted content
4. Wait for `y`/Enter to execute all lines, `n`/any other key to discard
5. Return to prompt

## Testing

### New test: tests/test_stsh.py

Runs on QEMU q35 with NVMe + disk. Login chain auto-login spawning stsh.

| Test | Verifies |
|------|----------|
| `test_prompt` | stsh starts, shows `root@aegis:/#` (CAP_DELEGATE held) |
| `test_basic_command` | `echo hello` produces `hello` |
| `test_pipeline` | `echo hello | cat` produces `hello` |
| `test_redirect` | `echo hello > /tmp/t && cat /tmp/t` produces `hello` |
| `test_caps_self` | `caps` shows own caps including CAP_DELEGATE and CAP_QUERY |
| `test_caps_pid` | `caps 1` shows init's capability set |
| `test_sandbox` | `sandbox -allow VFS_READ,VFS_WRITE -- cat /etc/motd` succeeds |
| `test_sandbox_denied` | `sandbox -allow VFS_READ -- /bin/httpd` fails (no NET_SOCKET) |
| `test_env_export` | `export FOO=bar && echo $FOO` produces `bar` |
| `test_history_up` | Send command, up-arrow, verify line matches |
| `test_exit_code` | `false; echo $?` produces `1` |
| `test_cd_prompt` | `cd /tmp` changes prompt to `root@aegis:/tmp#` |

12 tests, single QEMU boot.

### Test consolidation

As a final task: attempt to run existing `-machine pc` tests on `-machine q35`.
If output differences are acceptable (additional PCIE/IOAPIC/XHCI/NVMe/ext2
init lines), update the test harness to accept both. Goal: merge q35 tests
into a single `test_integrated_q35.py`, reducing total QEMU boots from 25+.

## Forward Constraints

1. **Grant builtin deferred to Phase 45 (capd).** stsh can query and restrict
   caps, but cannot grant caps to running processes until capd exists (requires
   IPC). The `grant` builtin prints "capd not running" until then.

2. **No scripting.** No if/for/while/case/functions. stsh v1 is interactive
   only. Scripting is v2.

3. **No command substitution.** `$(...)` and backticks not supported. v2.

4. **No glob expansion.** `*.c` is passed literally to commands. v2.

5. **No job control.** No bg/fg/Ctrl-Z. Single foreground pipeline only. v2.

6. **Hostname hardcoded.** No `gethostname` syscall. Always "aegis".

7. **CAP_TABLE_SIZE is 16.** The cap_mask array in sys_spawn is fixed at 16
   slots. If CAP_TABLE_SIZE changes, sys_spawn cap_mask must match.

8. **Tab completion is not capability-aware (v1).** Completes all visible
   files/commands regardless of permissions. Capability-aware filtering (only
   show accessible entries) is v2.

9. **sys_spawn now has 5 parameters.** Existing callers (lumen terminal) pass
   NULL for cap_mask to preserve current behavior.

10. **stsh is on ext2, not initrd.** Login and vigil remain on initrd (static,
    boot-critical). stsh is a dynamically linked binary on the ext2 disk.
    initrd's /bin/sh is unchanged for emergency/recovery use.
