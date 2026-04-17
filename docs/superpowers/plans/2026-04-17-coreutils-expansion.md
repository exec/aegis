# Coreutils Expansion (1.0.3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 20 new userspace utilities to Aegis 1.0.3, plus two small kernel patches that unblock them.

**Architecture:** Each utility is a standalone musl-dynamic-linked C binary in `user/bin/<name>/`, registered in `rootfs.manifest`, and exercised by a shared Rust integration test that boots the kernel in QEMU and drives each util through stsh. Two kernel patches land first: `envp` propagation in `sys_execve` (so `env`/`which` work), and a minimal `sethostname` syscall + persistent hostname (so `hostname` is writable). All 20 utils are then implemented in the same pattern: source → Makefile → manifest entry → test case → commit.

**Tech Stack:** musl libc 1.2.5 (dynamic), C99, `musl-gcc` cross-compiler, Rust integration tests via Vortex/QEMU harness, Aegis kernel syscalls.

---

## File Structure

**Created files** (one set per utility):
- `user/bin/<util>/main.c` — implementation, single file
- `user/bin/<util>/Makefile` — copy of the cat Makefile pattern

**Modified files** (each util adds one line):
- `rootfs.manifest` — `user/bin/<util>/<util>.elf  /bin/<util>`
- `tests/tests/coreutils_test.rs` — one assertion per util

**Kernel patches:**
- `kernel/syscall/sys_exec.c` — propagate envp through fork/execve
- `kernel/syscall/sys_impl.h` + `kernel/syscall/syscall.c` + new `kernel/syscall/sys_hostname.c` — sethostname/gethostname

**Test infrastructure (new):**
- `tests/tests/coreutils_test.rs` — single Rust integration test that boots the installer-test ISO (gives us autologin → stsh shell), runs every new util via `proc.send_keys()`, and asserts on serial output

---

## Per-Utility Task Template

Every utility task follows this five-step shape. The kernel patches and the test harness setup are exceptions and are spelled out in full.

1. Create `user/bin/<util>/main.c` with the full implementation (shown in each task).
2. Create `user/bin/<util>/Makefile` (identical to existing pattern, parameterized only on the binary name).
3. Build it: `cd user/bin/<util> && make` — expect `<util>.elf` produced, no warnings, no errors.
4. Append the manifest line and the test case (both shown).
5. Commit: `git add user/bin/<util>/ rootfs.manifest tests/tests/coreutils_test.rs && git commit -m "coreutils: add <util>"`.

The Makefile is the same shape every time, just with the binary name swapped. Reference (this is what every util's Makefile contains, with `<util>` substituted):

```makefile
MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none

<util>.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.elf
```

---

## Execution Notes

- All code must be built and tested on the x86 build box (`dylan@10.0.0.19`, key `~/.ssh/aegis/id_ed25519`, repo at `~/Developer/aegis`). musl-gcc is not available on Mac. Rsync edits, `touch` files to defeat preserved Mac timestamps, then `make`.
- After EVERY util is added, run the full test suite: `make installer-test-iso && AEGIS_INSTALLER_TEST_ISO=$PWD/build/aegis-installer-test.iso cargo test --manifest-path tests/Cargo.toml --test coreutils_test -- --nocapture --test-threads=1`. The whole suite should pass at every commit, not just at the end.
- Order matters: the kernel patches MUST land before utils that depend on them (`env`, `which`, `hostname`).

---

## Task 1: Kernel patch — envp propagation in execve

**Files:**
- Modify: `kernel/syscall/sys_exec.c` (envp copy + ELF stack build)
- Test: manual via QEMU once Task 4 (`env` util) exists

**Background:** Currently `sys_execve` ignores `envp_uptr` (`sys_exec.c:56`: `(void)envp_uptr; /* envp not yet supported */`). Library calls like `getenv()` always return NULL because the process starts with an empty environment. Argv handling in the same file is the model — copy strings from user, build a null-terminated `char *` array, push them onto the new process stack between argv and the auxv.

- [ ] **Step 1: Read the current argv handling for context**

Read `kernel/syscall/sys_exec.c` lines 70–115 (path copy + argv copy) and lines around the stack-build section (search for `/* 7. Build x86-64 SysV ABI initial stack */` — this is where envp goes between argv NULL and auxv).

- [ ] **Step 2: Extend the argbuf struct with envp storage**

Find the `execve_argbuf_t` definition (search the codebase: `grep -rn "execve_argbuf_t" kernel/`). It currently has `argv_bufs[64][256]` and `argv_ptrs[65]`. Add parallel envp storage of the same shape. Edit the struct definition file:

```c
typedef struct {
    char  argv_bufs[64][256];
    char *argv_ptrs[65];      /* +1 for NULL terminator */
    char  envp_bufs[64][256]; /* NEW */
    char *envp_ptrs[65];      /* NEW: +1 for NULL terminator */
} execve_argbuf_t;
```

- [ ] **Step 3: Copy envp from user space, mirroring the argv copy block**

In `sys_exec.c`, immediately after the argv copy block ends (around line 113 — `abuf->argv_ptrs[argc] = (char *)0;`), and before the `int argc2 = argc;` line, insert a parallel envp copy:

```c
/* 2b. Copy envp from user (<=64 entries, each <=255 bytes).
 * Same shape as argv above. envp_uptr may be NULL or point to a
 * NULL-terminated array of char* — no entries means empty environment. */
int envc = 0;
if (envp_uptr) {
    uint64_t ptr_addr = envp_uptr;
    while (envc < 64) {
        if (!user_ptr_valid(ptr_addr, 8))
            { ret = (uint64_t)-(int64_t)14; goto done; }
        uint64_t str_ptr;
        copy_from_user(&str_ptr, (const void *)(uintptr_t)ptr_addr, 8);
        if (!str_ptr) break;  /* NULL terminator */
        {
            uint64_t i;
            for (i = 0; i < 255; i++) {
                if (!user_ptr_valid(str_ptr + i, 1))
                    { ret = (uint64_t)-(int64_t)14; goto done; }
                char c;
                copy_from_user(&c,
                    (const void *)(uintptr_t)(str_ptr + i), 1);
                abuf->envp_bufs[envc][i] = c;
                if (c == '\0') break;
            }
        }
        abuf->envp_bufs[envc][255] = '\0';
        abuf->envp_ptrs[envc] = abuf->envp_bufs[envc];
        envc++;
        ptr_addr += 8;
    }
}
abuf->envp_ptrs[envc] = (char *)0;
```

- [ ] **Step 4: Push envp onto the new process stack**

Find the section that builds the user stack (search for `argv_ptrs` writes near the bottom of `sys_execve`). The current layout pushes `argc, argv[0..argc-1], NULL` then jumps directly to auxv. Insert envp pointers between the argv NULL and the auxv block. Pseudo-shape (find the exact code and integrate):

```c
/* After pushing argv NULL terminator, push envp pointers + NULL */
for (int i = 0; i < envc; i++)
    push_qword(abuf->envp_ptrs[i] - abuf->argv_bufs[0]
               + ARGV_USER_VA);     /* relocate kernel ptr to user VA */
push_qword(0);  /* envp NULL terminator */
/* Now push auxv as before */
```

(The exact relocation math depends on how argv pointers are already translated — match that pattern for envp. Read the surrounding code; do not invent a new scheme.)

- [ ] **Step 5: Verify the kernel still builds**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'cd ~/Developer/aegis && touch kernel/syscall/sys_exec.c && \
   PATH=/home/dylan/.cargo/bin:/usr/local/bin:/usr/bin:/bin make iso 2>&1 | tail -3'
```

Expected: `Writing to 'stdio:build/aegis.iso' completed successfully.`

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/sys_exec.c
git commit -m "kernel: propagate envp through sys_execve"
```

---

## Task 2: Kernel patch — sethostname + persistent hostname

**Files:**
- Create: `kernel/syscall/sys_hostname.c`
- Modify: `kernel/syscall/sys_impl.h` (add prototypes)
- Modify: `kernel/syscall/syscall.c` (dispatch)
- Modify: `kernel/arch/x86_64/uname.c` (use the persistent hostname instead of hardcoded value) — find the actual file with `grep -rn "nodename" kernel/`
- Modify: `kernel/Makefile` if it lists individual .c files (most kernel Makefiles glob — check first)

**Background:** Linux exposes `sethostname(2)` as syscall 170 and `gethostname(2)` is implemented in libc via `uname()`. We add 170 to the dispatch table; libc's `gethostname` already works once `uname.nodename` is sourced from a writable buffer.

- [ ] **Step 1: Find the current uname implementation**

```bash
grep -rn "nodename" /Users/dylan/Developer/aegis/kernel/ | grep -v build/
```

Note the file and the hardcoded value (likely `"aegis"`). This is the buffer we need to make mutable.

- [ ] **Step 2: Create sys_hostname.c with a static mutable buffer**

`kernel/syscall/sys_hostname.c`:

```c
/* sys_hostname.c — persistent hostname for sethostname/gethostname.
 *
 * The buffer lives in kernel BSS. uname() reads it; sethostname()
 * writes it. Bounded to 64 bytes (Linux NEW_UTS_LEN + null).
 * No persistence across reboots — fine for now; userspace can rerun
 * sethostname at boot from /etc/hostname if desired. */
#include "sys_impl.h"
#include "uaccess.h"
#include "cap.h"
#include "sched.h"
#include "proc.h"
#include <stddef.h>

#define HOSTNAME_MAX 64

static char s_hostname[HOSTNAME_MAX] = "aegis";

const char *
hostname_get(void)
{
    return s_hostname;
}

uint64_t
sys_sethostname(uint64_t name_uptr, uint64_t len)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    /* Setting the hostname is an admin-tier operation. We piggyback on
     * CAP_KIND_POWER (already used for reboot/shutdown). If a userspace
     * process holds POWER it's trusted enough to rename the box. */
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_POWER, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)1;  /* EPERM */
    if (len == 0 || len >= HOSTNAME_MAX)
        return (uint64_t)-(int64_t)22; /* EINVAL */
    char buf[HOSTNAME_MAX];
    uint64_t i;
    for (i = 0; i < len; i++) {
        if (!user_ptr_valid(name_uptr + i, 1))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        copy_from_user(&buf[i], (const void *)(uintptr_t)(name_uptr + i), 1);
    }
    buf[len] = '\0';
    /* Copy into the persistent buffer; no concurrent writers — protected
     * by the syscall serial-entry SMAP/SMEP barrier. */
    for (i = 0; i <= len; i++)
        s_hostname[i] = buf[i];
    return 0;
}
```

- [ ] **Step 3: Add prototypes to sys_impl.h**

Add to `kernel/syscall/sys_impl.h`:

```c
const char *hostname_get(void);
uint64_t sys_sethostname(uint64_t name_uptr, uint64_t len);
```

- [ ] **Step 4: Wire dispatch in syscall.c**

Add to the case dispatch in `kernel/syscall/syscall.c` (alongside the other syscall-number cases):

```c
case 170: return sys_sethostname(arg1, arg2);
```

- [ ] **Step 5: Patch uname to read the live hostname**

In the file from Step 1 (likely `kernel/arch/x86_64/uname.c`), change the hardcoded nodename memcpy to read from `hostname_get()`:

```c
const char *hn = hostname_get();
__builtin_strncpy(uts.nodename, hn, sizeof(uts.nodename) - 1);
uts.nodename[sizeof(uts.nodename) - 1] = '\0';
```

- [ ] **Step 6: Verify the kernel builds**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'cd ~/Developer/aegis && touch kernel/syscall/sys_hostname.c kernel/syscall/syscall.c kernel/arch/x86_64/uname.c && \
   PATH=/home/dylan/.cargo/bin:/usr/local/bin:/usr/bin:/bin make iso 2>&1 | tail -3'
```

Expected: ISO builds cleanly. If linker complains about `sys_hostname.c` not being compiled, find the kernel Makefile and either add the file or verify it's globbed.

- [ ] **Step 7: Commit**

```bash
git add kernel/syscall/sys_hostname.c kernel/syscall/sys_impl.h \
        kernel/syscall/syscall.c kernel/arch/x86_64/uname.c
git commit -m "kernel: add sethostname syscall + persistent hostname"
```

---

## Task 3: Test harness — coreutils_test.rs scaffold

**Files:**
- Create: `tests/tests/coreutils_test.rs`

**Background:** Single Rust integration test that boots `aegis-installer-test.iso` (autologin → drops into stsh as root). It runs every new util via `proc.send_keys()` and asserts that the expected output appears on serial. Each subsequent util task adds one assertion to this file.

- [ ] **Step 1: Create the test file with the boot harness only (no util assertions yet)**

`tests/tests/coreutils_test.rs`:

```rust
// End-to-end smoke test for the 1.0.3 coreutils additions.
//
// Boots the installer-test ISO (autologin as root → stsh prompt),
// drives each new utility via HMP sendkey, and asserts on serial
// output. Each util added in the 1.0.3 plan appends one block
// here. If any util's assertion fails, the test panics with the
// last 60 lines of serial.

use aegis_tests::{
    aegis_q35_gui_installer, wait_for_line, AegisHarness,
};
use std::path::PathBuf;
use std::time::Duration;

const BOOT_TIMEOUT_SECS: u64 = 240;

fn installer_test_iso() -> PathBuf {
    let val = std::env::var("AEGIS_INSTALLER_TEST_ISO")
        .unwrap_or_else(|_| "build/aegis-installer-test.iso".into());
    PathBuf::from(val)
}

#[tokio::test]
async fn coreutils_smoke() {
    let iso = installer_test_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_gui_installer(&PathBuf::from("/dev/null")), &iso)
            .await
            .expect("QEMU spawn failed");

    // Wait for stsh prompt (autologin lands here)
    wait_for_line(&mut stream, "[STSH] ready",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("[STSH] ready");

    // Per-utility assertions are appended below as utilities land.
    // Each block: send_keys("<cmd>\n").await; wait_for_line("<expected marker>"...)

    proc.kill().await.expect("kill");
}
```

- [ ] **Step 2: Verify stsh emits a `[STSH] ready` marker on shell start**

```bash
grep -n "STSH.*ready\|stsh: ready\|\[STSH\]" /Users/dylan/Developer/aegis/user/bin/stsh/*.c
```

If the marker doesn't exist, add a `dprintf(2, "[STSH] ready\n");` early in stsh `main()` after the prompt is set up. Without this marker the test has no reliable handoff signal between bastion-autologin and the first user-driven command.

- [ ] **Step 3: Verify the test compiles (it has no assertions yet but must build)**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'cd ~/Developer/aegis && touch tests/tests/coreutils_test.rs && \
   PATH=/home/dylan/.cargo/bin:/usr/local/bin:/usr/bin:/bin \
   cargo test --manifest-path tests/Cargo.toml --test coreutils_test --no-run 2>&1 | tail -5'
```

Expected: `Finished test [unoptimized + debuginfo]`.

- [ ] **Step 4: Commit**

```bash
git add tests/tests/coreutils_test.rs user/bin/stsh/  # if marker added
git commit -m "tests: add coreutils_test harness scaffold"
```

---

## Task 4: Add `sleep`

**Files:**
- Create: `user/bin/sleep/main.c`, `user/bin/sleep/Makefile`
- Modify: `rootfs.manifest`, `tests/tests/coreutils_test.rs`

- [ ] **Step 1: Write the implementation**

`user/bin/sleep/main.c`:

```c
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        dprintf(2, "usage: sleep <seconds>\n");
        return 1;
    }
    long secs = atol(argv[1]);
    if (secs < 0) secs = 0;
    struct timespec ts = { .tv_sec = secs, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    return 0;
}
```

- [ ] **Step 2: Write the Makefile** (use the template at the top, substitute `sleep` for `<util>`)

- [ ] **Step 3: Build it**

```bash
cd user/bin/sleep && make
ls -la sleep.elf
```

Expected: `sleep.elf` produced.

- [ ] **Step 4: Add to manifest**

Append to `rootfs.manifest` after the existing coreutils block:

```
user/bin/sleep/sleep.elf        /bin/sleep
```

- [ ] **Step 5: Add the test assertion**

Append before `proc.kill()` in `tests/tests/coreutils_test.rs`:

```rust
// sleep
proc.send_keys("sleep 0 && echo SLEEP_OK\n").await.unwrap();
wait_for_line(&mut stream, "SLEEP_OK", Duration::from_secs(5))
    .await.expect("sleep");
```

- [ ] **Step 6: Build ISO, run test**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'cd ~/Developer/aegis && touch rootfs.manifest tests/tests/coreutils_test.rs && \
   PATH=/home/dylan/.cargo/bin:/usr/local/bin:/usr/bin:/bin make installer-test-iso 2>&1 | tail -3 && \
   AEGIS_INSTALLER_TEST_ISO=$PWD/build/aegis-installer-test.iso \
   cargo test --manifest-path tests/Cargo.toml --test coreutils_test -- --nocapture --test-threads=1 2>&1 | tail -10'
```

Expected: `test result: ok. 1 passed`.

- [ ] **Step 7: Commit**

```bash
git add user/bin/sleep/ rootfs.manifest tests/tests/coreutils_test.rs
git commit -m "coreutils: add sleep"
```

---

## Task 5: Add `head`

`user/bin/head/main.c`:

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int s_n = 10;

static void
head_fd(int fd)
{
    char buf[1];
    int lines = 0;
    while (lines < s_n && read(fd, buf, 1) == 1) {
        write(1, buf, 1);
        if (buf[0] == '\n') lines++;
    }
}

int main(int argc, char **argv) {
    int i = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        s_n = atoi(argv[2]);
        if (s_n < 0) s_n = 0;
        i = 3;
    }
    if (i >= argc) {
        head_fd(0);
        return 0;
    }
    for (; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) { perror(argv[i]); return 1; }
        head_fd(fd);
        close(fd);
    }
    return 0;
}
```

Test assertion:

```rust
// head
proc.send_keys("head -n 1 /etc/passwd\n").await.unwrap();
// /etc/passwd's first line starts with "root:" — assert we see it
wait_for_line(&mut stream, "root:", Duration::from_secs(5))
    .await.expect("head");
```

Manifest line: `user/bin/head/head.elf        /bin/head`

Steps 1–7 follow the same pattern as Task 4 (write source, write Makefile from template, build, add manifest line, add test, run, commit). The commit message: `coreutils: add head`.

---

## Task 6: Add `tail`

`user/bin/tail/main.c`:

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_LINES 1024
#define MAX_LINE  4096

static int s_n = 10;
/* Ring buffer of the last N lines (literal copies, not pointers). */
static char s_ring[MAX_LINES][MAX_LINE];
static int  s_head = 0;
static int  s_count = 0;

static void
tail_fd(int fd)
{
    char  cur[MAX_LINE];
    int   ci = 0;
    char  c;
    while (read(fd, &c, 1) == 1) {
        if (ci < MAX_LINE - 1)
            cur[ci++] = c;
        if (c == '\n') {
            cur[ci] = '\0';
            int slot = s_head;
            int j;
            for (j = 0; j < ci; j++)
                s_ring[slot][j] = cur[j];
            s_ring[slot][ci] = '\0';
            s_head = (s_head + 1) % s_n;
            if (s_count < s_n) s_count++;
            ci = 0;
        }
    }
    if (ci > 0) {
        /* Trailing partial line */
        cur[ci] = '\0';
        int slot = s_head;
        int j;
        for (j = 0; j < ci; j++)
            s_ring[slot][j] = cur[j];
        s_ring[slot][ci] = '\0';
        s_head = (s_head + 1) % s_n;
        if (s_count < s_n) s_count++;
    }
}

static void
flush_ring(void)
{
    int start = (s_head + s_n - s_count) % s_n;
    int i;
    for (i = 0; i < s_count; i++) {
        const char *line = s_ring[(start + i) % s_n];
        int len = 0;
        while (line[len]) len++;
        write(1, line, (size_t)len);
    }
}

int main(int argc, char **argv) {
    int i = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        s_n = atoi(argv[2]);
        if (s_n <= 0) s_n = 10;
        if (s_n > MAX_LINES) s_n = MAX_LINES;
        i = 3;
    }
    if (i >= argc) {
        tail_fd(0);
    } else {
        for (; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { perror(argv[i]); return 1; }
            tail_fd(fd);
            close(fd);
        }
    }
    flush_ring();
    return 0;
}
```

Test:

```rust
// tail
proc.send_keys("tail -n 1 /etc/passwd\n").await.unwrap();
// /etc/passwd's last line — alice if installer-test created her
wait_for_line(&mut stream, "alice", Duration::from_secs(5))
    .await.expect("tail");
```

Manifest: `user/bin/tail/tail.elf        /bin/tail`. Commit: `coreutils: add tail`.

Note: -f mode is intentionally not implemented (would need ext2 .poll which doesn't exist).

---

## Task 7: Add `basename`

`user/bin/basename/main.c`:

```c
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(2, "usage: basename <path> [suffix]\n");
        return 1;
    }
    const char *p = argv[1];
    const char *base = p;
    /* Find the last '/' that isn't trailing */
    const char *end = p + strlen(p);
    while (end > p && *(end - 1) == '/') end--;
    const char *q;
    for (q = p; q < end; q++)
        if (*q == '/') base = q + 1;
    int len = (int)(end - base);
    if (len <= 0) {
        write(1, "/\n", 2);
        return 0;
    }
    /* Optional suffix removal */
    if (argc >= 3) {
        int slen = (int)strlen(argv[2]);
        if (slen <= len &&
            strncmp(base + len - slen, argv[2], (size_t)slen) == 0) {
            len -= slen;
        }
    }
    write(1, base, (size_t)len);
    write(1, "\n", 1);
    return 0;
}
```

Test:

```rust
// basename
proc.send_keys("basename /usr/local/bin/sleep\n").await.unwrap();
wait_for_line(&mut stream, "sleep", Duration::from_secs(5))
    .await.expect("basename");
```

Manifest: `user/bin/basename/basename.elf  /bin/basename`. Commit: `coreutils: add basename`.

---

## Task 8: Add `dirname`

`user/bin/dirname/main.c`:

```c
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        dprintf(2, "usage: dirname <path>\n");
        return 1;
    }
    const char *p = argv[1];
    int len = (int)strlen(p);
    /* Trim trailing slashes */
    while (len > 1 && p[len - 1] == '/') len--;
    /* Find last '/' */
    int last = -1;
    int i;
    for (i = 0; i < len; i++)
        if (p[i] == '/') last = i;
    if (last < 0) {
        write(1, ".\n", 2);
        return 0;
    }
    if (last == 0) {
        write(1, "/\n", 2);
        return 0;
    }
    /* Trim trailing slashes in the directory portion too */
    while (last > 0 && p[last - 1] == '/') last--;
    write(1, p, (size_t)last);
    write(1, "\n", 1);
    return 0;
}
```

Test:

```rust
// dirname
proc.send_keys("dirname /usr/local/bin/sleep\n").await.unwrap();
wait_for_line(&mut stream, "/usr/local/bin", Duration::from_secs(5))
    .await.expect("dirname");
```

Manifest: `user/bin/dirname/dirname.elf  /bin/dirname`. Commit: `coreutils: add dirname`.

---

## Task 9: Add `tee`

`user/bin/tee/main.c`:

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define MAX_OUT 16

int main(int argc, char **argv) {
    int append = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-a") == 0) {
        append = 1;
        i++;
    }
    int fds[MAX_OUT];
    int nfds = 0;
    for (; i < argc && nfds < MAX_OUT; i++) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int fd = open(argv[i], flags, 0644);
        if (fd < 0) { perror(argv[i]); continue; }
        fds[nfds++] = fd;
    }
    char buf[512];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
        int j;
        for (j = 0; j < nfds; j++)
            write(fds[j], buf, (size_t)n);
    }
    int j;
    for (j = 0; j < nfds; j++) close(fds[j]);
    return 0;
}
```

Test:

```rust
// tee
proc.send_keys("echo TEE_OK | tee /tmp/tee_test\n").await.unwrap();
wait_for_line(&mut stream, "TEE_OK", Duration::from_secs(5))
    .await.expect("tee echoes to stdout");
proc.send_keys("cat /tmp/tee_test\n").await.unwrap();
wait_for_line(&mut stream, "TEE_OK", Duration::from_secs(5))
    .await.expect("tee wrote file");
```

Manifest: `user/bin/tee/tee.elf            /bin/tee`. Commit: `coreutils: add tee`.

---

## Task 10: Add `env` (depends on Task 1)

`user/bin/env/main.c`:

```c
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern char **environ;

int main(int argc, char **argv) {
    /* Walk argv: while arg looks like KEY=VAL, set and consume.
     * First non-KEY=VAL is the command to exec. */
    int i = 1;
    while (i < argc && strchr(argv[i], '=') != NULL) {
        putenv(argv[i]);
        i++;
    }
    if (i >= argc) {
        /* No command — print environ */
        char **e;
        for (e = environ; *e; e++) {
            int len = (int)strlen(*e);
            write(1, *e, (size_t)len);
            write(1, "\n", 1);
        }
        return 0;
    }
    execvp(argv[i], &argv[i]);
    perror(argv[i]);
    return 127;
}
```

Test:

```rust
// env (requires Task 1: envp propagation)
proc.send_keys("env FOO=bar /bin/sh -c 'echo FOO=$FOO'\n").await.unwrap();
wait_for_line(&mut stream, "FOO=bar", Duration::from_secs(5))
    .await.expect("env propagates VAR=val");
```

Manifest: `user/bin/env/env.elf            /bin/env`. Commit: `coreutils: add env`.

---

## Task 11: Add `date`

`user/bin/date/main.c`:

```c
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        perror("clock_gettime");
        return 1;
    }
    time_t t = ts.tv_sec;
    struct tm tm;
    gmtime_r(&t, &tm);

    const char *fmt = "%a %b %e %H:%M:%S UTC %Y";
    if (argc >= 2 && argv[1][0] == '+')
        fmt = argv[1] + 1;

    char out[128];
    size_t n = strftime(out, sizeof(out), fmt, &tm);
    write(1, out, n);
    write(1, "\n", 1);
    return 0;
}
```

Test:

```rust
// date — assert we get any 4-digit year (UTC, not stable enough to match exactly)
proc.send_keys("date\n").await.unwrap();
wait_for_line(&mut stream, "UTC", Duration::from_secs(5))
    .await.expect("date prints UTC");
```

Manifest: `user/bin/date/date.elf          /bin/date`. Commit: `coreutils: add date`.

---

## Task 12: Add `hostname` (depends on Task 2)

`user/bin/hostname/main.c`:

```c
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>

#define SYS_SETHOSTNAME 170

int main(int argc, char **argv) {
    if (argc >= 2) {
        size_t len = strlen(argv[1]);
        long rc = syscall(SYS_SETHOSTNAME, argv[1], (long)len);
        if (rc != 0) {
            dprintf(2, "hostname: set failed (%ld)\n", rc);
            return 1;
        }
        return 0;
    }
    struct utsname u;
    if (uname(&u) != 0) { perror("uname"); return 1; }
    int n = (int)strlen(u.nodename);
    write(1, u.nodename, (size_t)n);
    write(1, "\n", 1);
    return 0;
}
```

Test:

```rust
// hostname
proc.send_keys("hostname\n").await.unwrap();
wait_for_line(&mut stream, "aegis", Duration::from_secs(5))
    .await.expect("hostname read");
```

Manifest: `user/bin/hostname/hostname.elf  /bin/hostname`. Commit: `coreutils: add hostname`.

---

## Task 13: Add `sync`

`user/bin/sync/main.c`:

```c
#include <unistd.h>

int main(void) {
    sync();
    return 0;
}
```

Test:

```rust
// sync
proc.send_keys("sync && echo SYNC_OK\n").await.unwrap();
wait_for_line(&mut stream, "SYNC_OK", Duration::from_secs(5))
    .await.expect("sync");
```

Manifest: `user/bin/sync/sync.elf          /bin/sync`. Commit: `coreutils: add sync`.

---

## Task 14: Add `tr`

`user/bin/tr/main.c`:

```c
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int delete_mode = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-d") == 0) { delete_mode = 1; i++; }

    if (delete_mode) {
        if (i + 1 != argc) {
            dprintf(2, "usage: tr -d <chars>\n");
            return 1;
        }
        const char *del = argv[i];
        char buf[1];
        while (read(0, buf, 1) == 1) {
            int drop = 0;
            const char *d;
            for (d = del; *d; d++)
                if (*d == buf[0]) { drop = 1; break; }
            if (!drop) write(1, buf, 1);
        }
        return 0;
    }

    if (i + 2 != argc) {
        dprintf(2, "usage: tr <set1> <set2>   |   tr -d <chars>\n");
        return 1;
    }
    const char *s1 = argv[i];
    const char *s2 = argv[i + 1];
    int s1l = (int)strlen(s1);
    int s2l = (int)strlen(s2);

    char buf[1];
    while (read(0, buf, 1) == 1) {
        int j;
        char c = buf[0];
        for (j = 0; j < s1l; j++) {
            if (s1[j] == buf[0]) {
                c = (j < s2l) ? s2[j] : s2[s2l - 1];
                break;
            }
        }
        write(1, &c, 1);
    }
    return 0;
}
```

Test:

```rust
// tr
proc.send_keys("echo HELLO | tr A-Z a-z\n").await.unwrap();
// Note: literal "A-Z a-z" handling (no class expansion). Adjust:
proc.send_keys("echo HELLO | tr 'HELO' 'helo'\n").await.unwrap();
wait_for_line(&mut stream, "hello", Duration::from_secs(5))
    .await.expect("tr translate");
```

Manifest: `user/bin/tr/tr.elf              /bin/tr`. Commit: `coreutils: add tr (literal sets)`.

---

## Task 15: Add `cut`

`user/bin/cut/main.c`:

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_FIELDS 64

int main(int argc, char **argv) {
    char delim = '\t';
    int fields[MAX_FIELDS];
    int nfields = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[++i][0];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            char *spec = argv[++i];
            char *tok = strtok(spec, ",");
            while (tok && nfields < MAX_FIELDS) {
                fields[nfields++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
        } else {
            break;
        }
    }
    if (nfields == 0) {
        dprintf(2, "usage: cut -d <c> -f <list> [file]\n");
        return 1;
    }

    int fd = (i < argc) ? open(argv[i], 0) : 0;
    if (fd < 0) { perror(argv[i]); return 1; }

    char line[4096];
    int li = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || li == sizeof(line) - 1) {
            line[li] = '\0';
            int field = 1;
            int start = 0;
            int j;
            int written = 0;
            for (j = 0; j <= li; j++) {
                if (line[j] == delim || line[j] == '\0') {
                    int k;
                    for (k = 0; k < nfields; k++) {
                        if (fields[k] == field) {
                            if (written) write(1, &delim, 1);
                            write(1, line + start, (size_t)(j - start));
                            written = 1;
                            break;
                        }
                    }
                    field++;
                    start = j + 1;
                }
            }
            write(1, "\n", 1);
            li = 0;
        } else {
            line[li++] = c;
        }
    }
    if (fd > 0) close(fd);
    return 0;
}
```

Test:

```rust
// cut
proc.send_keys("echo a:b:c | cut -d : -f 2\n").await.unwrap();
wait_for_line(&mut stream, "b", Duration::from_secs(5))
    .await.expect("cut field");
```

Manifest: `user/bin/cut/cut.elf            /bin/cut`. Commit: `coreutils: add cut`.

---

## Task 16: Add `expand`

`user/bin/expand/main.c`:

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

int main(int argc, char **argv) {
    int tab = 8;
    int i = 1;
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 't') {
        tab = atoi(argv[2]);
        if (tab <= 0) tab = 8;
        i = 3;
    }
    int fd = (i < argc) ? open(argv[i], 0) : 0;
    if (fd < 0) { perror(argv[i]); return 1; }
    int col = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\t') {
            int spaces = tab - (col % tab);
            int j;
            for (j = 0; j < spaces; j++) write(1, " ", 1);
            col += spaces;
        } else if (c == '\n') {
            write(1, &c, 1);
            col = 0;
        } else {
            write(1, &c, 1);
            col++;
        }
    }
    if (fd > 0) close(fd);
    return 0;
}
```

Test:

```rust
// expand
proc.send_keys("printf 'a\\tb' | expand -t 4\n").await.unwrap();
wait_for_line(&mut stream, "a   b", Duration::from_secs(5))
    .await.expect("expand tab");
```

Manifest: `user/bin/expand/expand.elf      /bin/expand`. Commit: `coreutils: add expand`.

---

## Task 17: Add `realpath`

`user/bin/realpath/main.c`:

```c
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(2, "usage: realpath <path>...\n");
        return 1;
    }
    int i;
    int rc = 0;
    char buf[PATH_MAX];
    for (i = 1; i < argc; i++) {
        if (realpath(argv[i], buf) == NULL) {
            perror(argv[i]);
            rc = 1;
            continue;
        }
        int n = 0;
        while (buf[n]) n++;
        write(1, buf, (size_t)n);
        write(1, "\n", 1);
    }
    return rc;
}
```

Test:

```rust
// realpath
proc.send_keys("realpath /bin/cat\n").await.unwrap();
wait_for_line(&mut stream, "/bin/cat", Duration::from_secs(5))
    .await.expect("realpath");
```

Manifest: `user/bin/realpath/realpath.elf  /bin/realpath`. Commit: `coreutils: add realpath`.

---

## Task 18: Add `stat`

`user/bin/stat/main.c`:

```c
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(2, "usage: stat <file>...\n");
        return 1;
    }
    int rc = 0;
    int i;
    for (i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) != 0) {
            perror(argv[i]);
            rc = 1;
            continue;
        }
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
                         "  File: %s\n  Size: %lld  Mode: %o  Uid: %u  Gid: %u  Inode: %lu\n",
                         argv[i],
                         (long long)st.st_size,
                         (unsigned)st.st_mode,
                         (unsigned)st.st_uid,
                         (unsigned)st.st_gid,
                         (unsigned long)st.st_ino);
        write(1, buf, (size_t)n);
    }
    return rc;
}
```

Test:

```rust
// stat
proc.send_keys("stat /bin/cat\n").await.unwrap();
wait_for_line(&mut stream, "Inode:", Duration::from_secs(5))
    .await.expect("stat");
```

Manifest: `user/bin/stat/stat.elf          /bin/stat`. Commit: `coreutils: add stat`.

---

## Task 19: Add `yes`

`user/bin/yes/main.c`:

```c
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *s = (argc >= 2) ? argv[1] : "y";
    int len = (int)strlen(s);
    char buf[512];
    int filled = 0;
    while (filled + len + 1 < (int)sizeof(buf)) {
        int j;
        for (j = 0; j < len; j++) buf[filled++] = s[j];
        buf[filled++] = '\n';
    }
    /* Write the prepacked buffer in a tight loop until SIGPIPE
     * (or the process is killed). */
    for (;;) {
        if (write(1, buf, (size_t)filled) <= 0) break;
    }
    return 0;
}
```

Test:

```rust
// yes — pipe through head -n 3, expect three "y" lines back
proc.send_keys("yes | head -n 3\n").await.unwrap();
// Crude check: just look for any line that's exactly "y"
wait_for_line(&mut stream, "y", Duration::from_secs(5))
    .await.expect("yes");
```

Manifest: `user/bin/yes/yes.elf            /bin/yes`. Commit: `coreutils: add yes`.

---

## Task 20: Add `test` and `[`

**Files:**
- Create: `user/bin/test/main.c`, `user/bin/test/Makefile` (Makefile builds BOTH `test.elf` and `[.elf` from the same source via a symlink-style copy)
- Modify: `rootfs.manifest`, `tests/tests/coreutils_test.rs`

`user/bin/test/main.c`:

```c
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

/* POSIX test/[ — supports the most common single-arg and two-arg forms.
 * Three-arg forms with -lt/-eq etc. for integer compare and = / != for
 * string compare. Doesn't pretend to handle ! / -a / -o / parens. */

static int
unary(const char *op, const char *arg)
{
    struct stat st;
    if (strcmp(op, "-e") == 0) return access(arg, F_OK) == 0 ? 0 : 1;
    if (strcmp(op, "-r") == 0) return access(arg, R_OK) == 0 ? 0 : 1;
    if (strcmp(op, "-w") == 0) return access(arg, W_OK) == 0 ? 0 : 1;
    if (strcmp(op, "-x") == 0) return access(arg, X_OK) == 0 ? 0 : 1;
    if (lstat(arg, &st) != 0) return 1;
    if (strcmp(op, "-f") == 0) return S_ISREG(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-d") == 0) return S_ISDIR(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0)
        return S_ISLNK(st.st_mode) ? 0 : 1;
    if (strcmp(op, "-s") == 0) return st.st_size > 0 ? 0 : 1;
    if (strcmp(op, "-n") == 0) return arg[0] != '\0' ? 0 : 1;
    if (strcmp(op, "-z") == 0) return arg[0] == '\0' ? 0 : 1;
    return 2; /* unknown */
}

static int
binary(const char *a, const char *op, const char *b)
{
    if (strcmp(op, "=")  == 0) return strcmp(a, b) == 0 ? 0 : 1;
    if (strcmp(op, "!=") == 0) return strcmp(a, b) != 0 ? 0 : 1;
    long la = atol(a), lb = atol(b);
    if (strcmp(op, "-eq") == 0) return la == lb ? 0 : 1;
    if (strcmp(op, "-ne") == 0) return la != lb ? 0 : 1;
    if (strcmp(op, "-lt") == 0) return la <  lb ? 0 : 1;
    if (strcmp(op, "-le") == 0) return la <= lb ? 0 : 1;
    if (strcmp(op, "-gt") == 0) return la >  lb ? 0 : 1;
    if (strcmp(op, "-ge") == 0) return la >= lb ? 0 : 1;
    return 2;
}

int main(int argc, char **argv) {
    /* Strip trailing ']' if invoked as `[` */
    int n = argc;
    if (n > 0 && strcmp(argv[0] + (strlen(argv[0]) > 0 ? strlen(argv[0]) - 1 : 0), "[") == 0) {
        if (n < 2 || strcmp(argv[n - 1], "]") != 0) {
            dprintf(2, "[: missing ]\n");
            return 2;
        }
        n--;
    }
    if (n == 1) return 1;                          /* test     → false */
    if (n == 2) return argv[1][0] != '\0' ? 0 : 1; /* test STR → STR nonempty */
    if (n == 3) return unary(argv[1], argv[2]);
    if (n == 4) return binary(argv[1], argv[2], argv[3]);
    dprintf(2, "test: too many arguments\n");
    return 2;
}
```

`user/bin/test/Makefile`:

```makefile
MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none

all: test.elf bracket.elf

test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

bracket.elf: test.elf
	cp test.elf bracket.elf

clean:
	rm -f *.elf
```

Manifest (TWO lines):

```
user/bin/test/test.elf          /bin/test
user/bin/test/bracket.elf       /bin/[
```

Test:

```rust
// test / [
proc.send_keys("test -f /bin/cat && echo TEST_F_OK\n").await.unwrap();
wait_for_line(&mut stream, "TEST_F_OK", Duration::from_secs(5))
    .await.expect("test -f");
proc.send_keys("[ 5 -gt 3 ] && echo BRACKET_OK\n").await.unwrap();
wait_for_line(&mut stream, "BRACKET_OK", Duration::from_secs(5))
    .await.expect("[ -gt ]");
```

Commit: `coreutils: add test / [`.

---

## Task 21: Add `find`

`user/bin/find/main.c`:

```c
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>

static const char *s_pattern = NULL;  /* -name pattern, NULL = match all */

static void
walk(const char *path)
{
    /* Print this entry if it matches */
    const char *base = path;
    const char *p;
    for (p = path; *p; p++)
        if (*p == '/' && *(p + 1)) base = p + 1;
    if (!s_pattern || fnmatch(s_pattern, base, 0) == 0) {
        write(1, path, strlen(path));
        write(1, "\n", 1);
    }
    /* Descend if directory */
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n <= 0 || n >= (int)sizeof(child)) continue;
        walk(child);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const char *root = ".";
    int i = 1;
    if (i < argc && argv[i][0] != '-') { root = argv[i]; i++; }
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            s_pattern = argv[++i];
        }
    }
    walk(root);
    return 0;
}
```

Test:

```rust
// find
proc.send_keys("find /bin -name 'cat'\n").await.unwrap();
wait_for_line(&mut stream, "/bin/cat", Duration::from_secs(10))
    .await.expect("find -name");
```

Manifest: `user/bin/find/find.elf          /bin/find`. Commit: `coreutils: add find`.

---

## Task 22: Add `which`

`user/bin/which/main.c`:

```c
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(2, "usage: which <command>...\n");
        return 1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";  /* Aegis fallback */
    int rc = 0;
    int i;
    for (i = 1; i < argc; i++) {
        const char *p = path;
        int found = 0;
        while (*p) {
            const char *colon = strchr(p, ':');
            int seg_len = colon ? (int)(colon - p) : (int)strlen(p);
            char candidate[512];
            int n = seg_len;
            if (n >= (int)sizeof(candidate) - 1 -
                       (int)strlen(argv[i]) - 2) n = 0;
            int k;
            for (k = 0; k < seg_len; k++) candidate[k] = p[k];
            candidate[seg_len] = '/';
            int al = (int)strlen(argv[i]);
            int j;
            for (j = 0; j < al; j++) candidate[seg_len + 1 + j] = argv[i][j];
            candidate[seg_len + 1 + al] = '\0';
            if (access(candidate, X_OK) == 0) {
                write(1, candidate, (size_t)(seg_len + 1 + al));
                write(1, "\n", 1);
                found = 1;
                break;
            }
            if (!colon) break;
            p = colon + 1;
        }
        if (!found) rc = 1;
    }
    return rc;
}
```

Test:

```rust
// which
proc.send_keys("which cat\n").await.unwrap();
wait_for_line(&mut stream, "/bin/cat", Duration::from_secs(5))
    .await.expect("which");
```

Manifest: `user/bin/which/which.elf        /bin/which`. Commit: `coreutils: add which`.

---

## Task 23: Add `uniq`

`user/bin/uniq/main.c`:

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int fd = 0;
    if (argc >= 2) {
        fd = open(argv[1], 0);
        if (fd < 0) { perror(argv[1]); return 1; }
    }
    char prev[4096] = "";
    int prev_len = -1;
    char cur[4096];
    int  cur_len = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || cur_len == sizeof(cur) - 1) {
            cur[cur_len] = '\0';
            if (prev_len < 0 || cur_len != prev_len ||
                memcmp(cur, prev, (size_t)cur_len) != 0) {
                write(1, cur, (size_t)cur_len);
                write(1, "\n", 1);
                memcpy(prev, cur, (size_t)cur_len);
                prev[cur_len] = '\0';
                prev_len = cur_len;
            }
            cur_len = 0;
        } else {
            cur[cur_len++] = c;
        }
    }
    if (fd > 0) close(fd);
    return 0;
}
```

Test:

```rust
// uniq
proc.send_keys("printf 'a\\na\\nb\\n' | uniq\n").await.unwrap();
wait_for_line(&mut stream, "b", Duration::from_secs(5))
    .await.expect("uniq output");
```

Manifest: `user/bin/uniq/uniq.elf          /bin/uniq`. Commit: `coreutils: add uniq`.

---

## Task 24: Final integration — ISO + full test pass + release-notes update

**Files:**
- Modify: `aegissite/_posts/2026-04-18-v1-0-3-release.md` (replace the placeholder section)
- No code changes

- [ ] **Step 1: Run the full coreutils suite, all 20 utils**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'cd ~/Developer/aegis && \
   PATH=/home/dylan/.cargo/bin:/usr/local/bin:/usr/bin:/bin make installer-test-iso && \
   AEGIS_INSTALLER_TEST_ISO=$PWD/build/aegis-installer-test.iso \
   cargo test --manifest-path tests/Cargo.toml --test coreutils_test -- --nocapture --test-threads=1 2>&1 | tail -20'
```

Expected: `test result: ok. 1 passed`. If anything fails, fix the failing util before continuing — do not bundle multiple fixes.

- [ ] **Step 2: Run all other tests to make sure nothing regressed**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'cd ~/Developer/aegis && \
   PATH=/home/dylan/.cargo/bin:/usr/local/bin:/usr/bin:/bin \
   AEGIS_INSTALLER_TEST_ISO=$PWD/build/aegis-installer-test.iso \
   AEGIS_ISO=$PWD/build/aegis.iso \
   cargo test --manifest-path tests/Cargo.toml -- --nocapture --test-threads=1 2>&1 | tail -30'
```

Expected: every test green except for any that were already known-flaky before this work began.

- [ ] **Step 3: Build the production ISO**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'cd ~/Developer/aegis && \
   PATH=/home/dylan/.cargo/bin:/usr/local/bin:/usr/bin:/bin make iso 2>&1 | tail -3 && \
   ls -la build/aegis.iso'
```

- [ ] **Step 4: Pull ISO to local for flashing**

```bash
rsync -azP -e "ssh -i ~/.ssh/aegis/id_ed25519" \
  dylan@10.0.0.19:~/Developer/aegis/build/aegis.iso \
  /Users/dylan/Developer/aegis/build/aegis.iso
shasum -a 256 /Users/dylan/Developer/aegis/build/aegis.iso
```

- [ ] **Step 5: Replace the TBD section in the 1.0.3 release post**

Edit `/Users/dylan/Developer/aegissite/_posts/2026-04-18-v1-0-3-release.md`. Replace the `## NEW FEATURE TBD — placeholder section` block with a real section titled `## 20 new coreutils` that lists every utility added (group by purpose: file/text/info/shell-helper) and mentions the two enabling kernel patches (envp propagation, sethostname). Match the tone of the rest of the post — conversational, owns the gaps (e.g. "tr only does literal sets, no [:alpha:] classes yet").

- [ ] **Step 6: Final commit**

```bash
git add aegissite/_posts/2026-04-18-v1-0-3-release.md
git commit -m "blog: 1.0.3 — 20 new coreutils + envp + sethostname"
```

---

## Acceptance Criteria

- [ ] All 20 new utility binaries are in `/bin` on a freshly-installed Aegis system.
- [ ] `coreutils_test.rs` passes 1/1 against `aegis-installer-test.iso`.
- [ ] No previously-green test in `tests/tests/` regressed.
- [ ] `env FOO=bar /bin/sh -c 'echo $FOO'` prints `bar` (envp propagation works).
- [ ] `hostname newname && hostname` prints `newname` when the caller has CAP_KIND_POWER (sethostname works).
- [ ] The 1.0.3 release blog post has a real `## 20 new coreutils` section in place of the placeholder.
- [ ] A new production ISO has been pulled to `/Users/dylan/Developer/aegis/build/aegis.iso`.
