# Phase 46: Bastion — Graphical Display Manager + Code Separation

**Date:** 2026-03-31
**Status:** Approved
**Depends on:** Phase 45 (capd), Phase 37 (Lumen), Phase 39 (Glyph)

---

## Overview

Phase 46 introduces Bastion, a graphical display manager (login screen) for Aegis,
and separates the GUI codebase into clean library boundaries. Bastion replaces the
unauthenticated graphical boot path — currently Lumen spawns root shells with zero
authentication. Bastion presents a graphical login form, authenticates via shared
`libauth.a`, then spawns Lumen as the authenticated user's session.

### Goals

1. **Code separation** — extract `libcitadel.a` (desktop shell) and `libauth.a` (authentication) as libraries
2. **Bastion binary** — graphical display manager with login, logout, and lock/unlock
3. **Vigil integration** — Bastion replaces Lumen as the graphical-mode Vigil service
4. **Session lifecycle** — greeter → authenticate → session → lock/logout → greeter

---

## Component 1: Code Separation

### libglyph.a (no changes)

Widget toolkit. Already a separate static library at `user/glyph/`. Buttons, labels,
text fields, layout containers, window chrome. No framebuffer awareness.

### libcitadel.a (new library)

Desktop shell components extracted from Lumen:

| File | Extracted from | Contents |
|------|---------------|----------|
| `dock.c` / `dock.h` | `lumen/dock.c` | Frosted-glass dock, icon rendering, click handling |
| `topbar.c` / `topbar.h` | `lumen/main.c` (topbar_draw) | Top bar with Aegis label + clock |
| `desktop.c` / `desktop.h` | `lumen/main.c` (desktop callbacks) | Desktop icons, context menu, wallpaper loading |
| `theme.h` | New | Shared color constants, font sizes, spacing |

**Source directory:** `user/citadel/`
**Build output:** `user/citadel/libcitadel.a`

### libauth.a (new library)

Authentication logic extracted from `/bin/login`:

| Function | Purpose |
|----------|---------|
| `auth_check(username, password, *uid, *gid, *home, *shell)` | Verify credentials against /etc/passwd + /etc/shadow |
| `auth_set_identity(uid, gid)` | Call setuid/setgid |
| `auth_setup_env(uid, gid, home, shell, username)` | Set HOME, USER, LOGNAME, SHELL, PATH |
| `auth_grant_shell_caps()` | Pre-register CAP_DELEGATE + CAP_QUERY as exec_caps |

**Source directory:** `user/libauth/`
**Build output:** `user/libauth/libauth.a`

Both `/bin/login` and `/bin/bastion` link `libauth.a`. Login becomes a thin
wrapper around the library. Auth code exists in exactly one place.

### /bin/lumen (slimmed)

Lumen links `libglyph.a` + `libcitadel.a`. The compositor code (framebuffer
ownership, window management, input dispatch, terminal spawning, event loop)
stays in `user/lumen/`. The shell code (dock, taskbar, desktop icons) moves
to `libcitadel.a` but is still linked into the Lumen binary.

---

## Component 2: Bastion Binary

**Binary:** `/bin/bastion`
**Source:** `user/bastion/`
**Links:** `libglyph.a` + `libauth.a`
**Does NOT link:** `libcitadel.a`

### Rendering

Bastion maps the framebuffer directly via `sys_fb_map` (no compositor). It
renders Glyph widgets to an offscreen backbuffer, then blits to the
framebuffer (double-buffered).

- Dark background (#1A1A2E)
- Aegis image logo centered above the form (loaded from `/usr/share/logo.raw` or embedded)
- ~400px wide centered panel
- Username label + text field
- Password label + text field (masked with `*`)
- "Login" button
- Error text area (red, hidden until failure)
- Keyboard: Tab cycles focus, Enter submits
- Mouse: click fields/button via `/dev/mouse` (non-blocking)

The same form is used for both greeter and lock screen:
- **Greeter mode:** Logo + both fields editable + "Login" button
- **Lock mode:** Logo + username pre-filled and read-only + "Unlock" button + "Locked" status text

### Capabilities

Bastion requests from capd at startup:
- `AUTH` (4) — read /etc/shadow
- `CAP_GRANT` (5) — pre-register exec_caps for shell
- `CAP_DELEGATE` (13) — delegate to child sessions
- `CAP_QUERY` (14) — delegate to child sessions
- `SETUID` (6) — change identity after auth

capd policy: `/etc/aegis/capd.d/bastion.policy` → `allow AUTH CAP_GRANT CAP_DELEGATE CAP_QUERY SETUID`

---

## Component 3: Session Lifecycle

### Greeter Flow (login)

```
Vigil spawns /bin/bastion (graphical mode)
  │
  Bastion requests caps from capd
  Bastion maps framebuffer, draws login screen
  │
  User types username + password, clicks Login
  │
  auth_check() validates against /etc/shadow
  ├── Failure: show error, allow retry (max 3)
  └── Success:
        auth_set_identity(uid, gid)
        auth_grant_shell_caps()  // CAP_DELEGATE + CAP_QUERY as exec_caps
        sys_spawn("/bin/lumen", ..., env)
        │
        Bastion enters waitpid(lumen_pid)  ← blocked, zero CPU
        │
        [Lumen session runs]
        │
        Lumen exits (logout or crash)
        waitpid returns
        │
        Bastion clears framebuffer
        Bastion loops back to greeter (fresh login)
```

### Lock Flow (Win+L)

```
User presses Win+L in Lumen
  │
  Lumen catches Win+L
  Lumen freezes input dispatch (ignores kbd/mouse)
  Lumen sends SIGUSR1 to parent PID (Bastion)
  │
  Bastion's SIGUSR1 handler sets locked=1
  waitpid returns with EINTR
  │
  Bastion draws lock screen:
    - Same form as greeter
    - Logo displayed
    - Username pre-filled, read-only
    - "Unlock" button instead of "Login"
  │
  User authenticates (same user only)
  ├── Failure: show error, retry
  └── Success:
        Bastion sends SIGUSR2 to Lumen PID
        Lumen resumes input dispatch
        Bastion returns to waitpid
```

### Logout Flow

The user triggers logout from within Lumen (future: menu item, or closing all
windows). Lumen exits normally. Bastion's waitpid returns, Bastion shows the
greeter again. All of Lumen's child processes die when Lumen exits (they are
in the same session).

### Process Death

- Bastion crashes → Vigil respawns → fresh greeter (all session state lost)
- Lumen crashes → waitpid returns → Bastion shows greeter (re-login required)
- capd unavailable → Bastion retries 3x, then shows error and waits

---

## Component 4: Vigil + capd Changes

### Vigil Service Descriptors

**Add Bastion:**
- `/etc/vigil/services/bastion/run` → `/bin/bastion`
- `/etc/vigil/services/bastion/policy` → `respawn`
- `/etc/vigil/services/bastion/mode` → `graphical`

**Remove Lumen from Vigil.** Bastion spawns Lumen directly. Lumen is no longer
a Vigil-supervised service. This means:
- Delete `/etc/vigil/services/lumen/` directory from ext2 image
- Lumen's crash recovery is handled by Bastion (shows greeter), not Vigil

**Getty (text mode) unchanged.** `/etc/vigil/services/getty/` stays as-is.
Text mode boot path is functionally identical.

### capd Policy

Add `/etc/aegis/capd.d/bastion.policy`:
```
allow AUTH CAP_GRANT CAP_DELEGATE CAP_QUERY SETUID
```

Same as `login.policy` — Bastion does the same auth work.

Remove `lumen.policy` — Lumen no longer requests caps from capd directly.
Bastion grants Lumen its caps via exec_caps at spawn time.

---

## Component 5: Testing

### Boot oracle

No changes to `tests/expected/boot.txt`. Bastion runs only in graphical mode.
The boot oracle uses `boot=text quiet`, so the text-mode path (getty → login)
is exercised. Bastion never starts during `make test`.

### Compile-time verification

- `libauth.a` builds clean
- `libcitadel.a` builds clean
- `/bin/login` links `libauth.a`, text-mode login still works
- `/bin/lumen` links `libcitadel.a`, graphical mode still works
- `/bin/bastion` links `libglyph.a` + `libauth.a`, builds clean
- All existing tests pass (text-mode path unchanged)

### Manual verification (bare-metal / QEMU VGA)

- Boot `graphical` → Bastion login screen with logo
- Authenticate → Lumen desktop appears
- Win+L → lock screen with logo, username pre-filled
- Unlock → desktop resumes
- Close Lumen → greeter reappears

---

## Forward Constraints

1. **Lumen → liblumen.a (future).** Long-term, Lumen's compositor logic should be
   extracted into `liblumen.a`. Bastion would then link `liblumen.a` + `libglyph.a` +
   `libcitadel.a` + `libauth.a` — one binary owns the entire graphical session.
   No framebuffer handoff between processes. This is a future phase, not Phase 46.
   **This is the target architecture. Record in CLAUDE.md.**

2. **Per-user capability policy (future).** Bastion's auth logic will eventually
   decide which caps to grant based on the authenticated user and the login route
   (local display vs SSH). Root from local display gets admin caps; non-root doesn't.
   The mechanism is already in place (exec_caps + sys_cap_grant), the policy logic
   is future work.

3. **No logout UI yet.** Lumen has no "Log out" menu item. The user can only log
   out by killing Lumen (Ctrl+Alt+Backspace or similar — not yet implemented).
   A logout menu item in Citadel's context menu is future work.

4. **Logo file must exist.** Bastion loads the Aegis logo from a raw pixel file.
   If missing, Bastion falls back to a text "AEGIS" label. The logo conversion
   tool (PNG → raw BGRA) must be added to the build.

5. **Bastion runs as root.** Bastion starts as root (PID from Vigil), authenticates,
   then calls `setuid/setgid` before spawning Lumen. After setuid, Bastion itself
   runs as the authenticated user. On lock screen, it re-authenticates the same user.
   On logout, Bastion exits and Vigil respawns a fresh root Bastion.

6. **Single user session.** No multi-seat, no fast user switching. One user at a time.

7. **fb_lock contention.** Both Bastion (greeter/lock) and Lumen (session) use
   `sys_fb_map`. When Bastion draws the lock screen while Lumen is frozen, both
   have the framebuffer mapped. This is safe — Lumen is frozen (not writing) and
   Bastion overwrites the entire framebuffer. On unlock, Lumen resumes and its
   next composite overwrites Bastion's lock screen.
