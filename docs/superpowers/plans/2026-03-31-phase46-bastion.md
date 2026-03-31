# Phase 46: Bastion + Code Separation — Implementation Plan

**Spec:** `docs/superpowers/specs/2026-03-31-phase46-bastion-design.md`
**Date:** 2026-03-31

---

## Task 1: Extract libauth.a from login

**Files:** `user/libauth/auth.c`, `user/libauth/auth.h`, `user/libauth/Makefile` (new),
`user/login/main.c` (refactor)

1. Create `user/libauth/` directory
2. Write `auth.h` with public API:
   - `int auth_check(const char *user, const char *pass, int *uid, int *gid, char *home, int homelen, char *shell, int shelllen)` — returns 0 on success
   - `int auth_set_identity(int uid, int gid)` — setuid + setgid
   - `void auth_grant_shell_caps(void)` — pre-register CAP_DELEGATE + CAP_QUERY exec_caps
   - `int capd_request(unsigned int kind)` — connect to capd, request a cap
3. Write `auth.c` — move `lookup_passwd`, `lookup_shadow`, `crypt` verification,
   `capd_request`, `readline` (as `auth_readline`) from login/main.c
4. Write `Makefile` — builds `libauth.a` via `ar rcs`
5. Refactor `user/login/main.c` — remove extracted functions, `#include "auth.h"`,
   call `auth_check`, `auth_set_identity`, `auth_grant_shell_caps`, `capd_request`
6. Update login Makefile to link `-L../libauth -lauth`

**Verify:** `make -C user/libauth && make -C user/login` both compile clean.

**Time:** ~5 minutes

---

## Task 2: Extract libcitadel.a from Lumen

**Files:** `user/citadel/dock.c`, `user/citadel/dock.h`, `user/citadel/topbar.c`,
`user/citadel/topbar.h`, `user/citadel/desktop.c`, `user/citadel/desktop.h`,
`user/citadel/theme.h`, `user/citadel/Makefile` (new)

1. Create `user/citadel/` directory
2. Move `user/lumen/dock.c` → `user/citadel/dock.c`, `dock.h` → `user/citadel/dock.h`
3. Extract topbar drawing from `lumen/main.c` into `user/citadel/topbar.c` / `topbar.h`
   - `topbar_draw(surface_t *s, int w, const char *clock_str)` and associated code
4. Extract desktop callbacks from `lumen/main.c` into `user/citadel/desktop.c` / `desktop.h`
   - `desktop_draw_cb`, `overlay_draw_cb`, wallpaper loading, context menu
5. Create `user/citadel/theme.h` — shared colors (DOCK_BG, TOPBAR_BG, etc.)
6. Write `Makefile` — compiles all `.c` files into `libcitadel.a`. Includes
   `-I../glyph` for `<glyph.h>`.
7. Update `user/lumen/Makefile` — add `-I../citadel -L../citadel -lcitadel` and
   remove dock.c from SRCS. Add citadel include path.
8. Update `user/lumen/main.c` — replace inline topbar/desktop code with calls
   to `libcitadel.a` functions via `#include` of citadel headers.

**Verify:** `make -C user/citadel && make -C user/lumen` both compile clean.

**Time:** ~5 minutes

---

## Task 3: Bastion binary — framebuffer + rendering

**Files:** `user/bastion/main.c`, `user/bastion/Makefile` (new)

1. Create `user/bastion/` directory
2. Write `Makefile` — links `libglyph.a` + `libauth.a` + `-lcrypt`
3. Write `main.c` scaffolding:
   - `sys_fb_map` to get framebuffer
   - Allocate backbuffer
   - Set stdin to raw mode (VMIN=0), open `/dev/mouse` non-blocking
   - Logo loading from `/usr/share/logo.raw` (fallback: skip if missing)
   - `draw_login_form(surface_t *bb, int mode)` — renders the form:
     - Dark background fill
     - Logo image centered
     - Glyph text fields (username, password)
     - Glyph button ("Login" or "Unlock")
     - Error label (hidden by default)
   - `blit_to_fb()` — memcpy backbuffer → framebuffer
   - Input polling loop: read kbd + mouse, dispatch to Glyph widgets
   - Tab key cycles focus between username/password/button
   - Enter triggers submit

**Verify:** Compiles clean. No functional test yet (graphical only).

**Time:** ~5 minutes

---

## Task 4: Bastion binary — authentication + session spawn

**Files:** `user/bastion/main.c` (extend)

1. Add capd capability requests at startup (AUTH, CAP_GRANT, CAP_DELEGATE,
   CAP_QUERY, SETUID) via `capd_request()` from libauth.a
2. Login button handler:
   - Get username/password from Glyph text fields
   - Call `auth_check(username, password, &uid, &gid, home, shell)`
   - On failure: set error label text, clear password field, max 3 attempts
   - On success: `auth_set_identity(uid, gid)`, `auth_grant_shell_caps()`,
     set up env (HOME, USER, PATH, TERM), `sys_spawn("/bin/lumen", ...)`
3. Session loop after spawn:
   - `waitpid(lumen_pid, &status, 0)` — blocks
   - On return: clear framebuffer, reset form, loop back to greeter
4. Signal handler for SIGUSR1 (lock):
   - Set `s_locked = 1` flag
   - Store `s_lumen_pid` for SIGUSR2 on unlock

**Verify:** Compiles clean.

**Time:** ~4 minutes

---

## Task 5: Bastion binary — lock/unlock flow

**Files:** `user/bastion/main.c` (extend), `user/lumen/main.c` (add Win+L handler)

1. In Bastion:
   - Install SIGUSR1 handler via `sigaction` (sets `s_locked` flag)
   - In main loop: after `waitpid` returns with EINTR + `s_locked`:
     - Remap framebuffer if needed
     - Draw lock screen (same form, username pre-filled and disabled, "Unlock" button)
     - Authenticate against stored username only
     - On success: send `SIGUSR2` to `s_lumen_pid`, clear `s_locked`, return to waitpid
2. In Lumen (`main.c`):
   - Detect Win+L keypress (Super key + L, or fallback Ctrl+Alt+L)
   - Set `s_input_frozen = 1` — skip all kbd/mouse dispatch in event loop
   - Send `SIGUSR1` to `getppid()` (Bastion)
   - Install SIGUSR2 handler that clears `s_input_frozen`
   - When `s_input_frozen`: continue compositing (so the screen isn't black)
     but ignore all input events

**Verify:** Compiles clean. Functional test requires graphical boot.

**Time:** ~4 minutes

---

## Task 6: Vigil + capd + Makefile integration

**Files:** `Makefile`, Vigil service descriptors

1. Add `libauth.a` build rule to Makefile
2. Add `libcitadel.a` build rule to Makefile
3. Add `bastion.elf` build rule — depends on `libglyph.a`, `libauth.a`
4. Add `bastion.elf` to `DISK_USER_BINS`
5. Add `bastion.elf` to rootfs write + chmod sections
6. Add Vigil service descriptor for Bastion (mode=graphical, respawn)
7. Remove Vigil service descriptor for Lumen (Bastion spawns Lumen now)
8. Add capd policy: `/etc/aegis/capd.d/bastion.policy`
9. Remove capd policy: `lumen.policy` (Lumen no longer talks to capd)
10. Add logo conversion: `tools/convert-logo.py` converts PNG → raw BGRA,
    write to `/usr/share/logo.raw` in rootfs (or embed fallback)

**Verify:** `make` builds everything. rootfs includes bastion, capd policies correct.

**Time:** ~5 minutes

---

## Task 7: Build + make test on x86 box

**Files:** All

1. Sync to x86 box
2. `make clean && make test`
3. Verify: boot oracle passes (text mode unchanged), all existing tests pass
4. Fix any build errors or test regressions

**Verify:** `make test` GREEN (same pass count as before — Bastion only affects graphical mode).

**Time:** ~5 minutes

---

## Task 8: Update CLAUDE.md build status

**Files:** `.claude/CLAUDE.md`

1. Add Phase 46 to build status table
2. Add Phase 46 forward constraints section
3. Update "Last updated" timestamp

**Verify:** Documentation complete.

**Time:** ~2 minutes

---

## Task Summary

| # | Task | Est. | Depends on |
|---|------|------|------------|
| 1 | Extract libauth.a | 5 min | — |
| 2 | Extract libcitadel.a | 5 min | — |
| 3 | Bastion rendering | 5 min | 1 |
| 4 | Bastion auth + session | 4 min | 3 |
| 5 | Lock/unlock flow | 4 min | 4 |
| 6 | Makefile + Vigil + capd | 5 min | 1, 2, 3 |
| 7 | Build + make test | 5 min | all |
| 8 | CLAUDE.md update | 2 min | 7 |

**Total:** ~35 minutes
