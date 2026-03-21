# Phase 8: SMAP + User Pointer Validation Design

## Goal

Close the user-pointer security hole in `sys_write`: add a bounds check that
returns EFAULT for kernel-space addresses, enable Supervisor Mode Access
Prevention (SMAP) so accidental kernel→user dereferences fault at the hardware
level, and wrap the intentional user-memory access with `stac`/`clac` so the
syscall path continues to work correctly under SMAP.

---

## Background

`sys_write` currently dereferences `arg2` (a user-supplied virtual address)
without any validation. A malicious process can pass a kernel-space address and
read arbitrary kernel memory through `printk`. The comment in `syscall.c`
documents this as a known Phase 5 security debt.

Two complementary protections are needed:

- **Bounds check** — software enforcement; catches intentional attacks.
  Returns EFAULT (`-14`, Linux convention, required for future musl port).
- **SMAP** — hardware enforcement; catches accidental kernel→user dereferences
  (bugs, not attacks). When `CR4.SMAP=1`, any ring-0 access to a user-mode
  page (U/S=1 in PTE) causes a #PF unless `RFLAGS.AC=1`. The `stac`
  instruction sets AC (re-enables access); `clac` clears it.

SMAP has been present since Intel Broadwell (2014) and AMD Zen (2017). We
target modern hardware and VMs, but the design detects and skips gracefully on
older CPUs rather than panicking — the bounds check remains effective either way.

**Why `stac`/`clac` are required:** enabling SMAP without them causes an
immediate #PF on the first `s[i]` dereference in `sys_write`, since `s` points
into a user-mode page. The `stac`/`clac` window is safe because `SFMASK` keeps
`IF=0` during syscall dispatch — no interrupt can fire with AC set. The window
must be as narrow as possible: bracket only the single user-memory load, not
the surrounding `printk` call. This is both a correctness requirement (limits
the AC window) and a security practice (never call code that may dereference
user pointers while AC is already set).

---

## Architecture

### New Module: `kernel/arch/x86_64/arch_smap.c`

```c
#include "printk.h"
#include <stdint.h>

static int
cpuid_smap_supported(void)
{
    uint32_t ebx;
    /* CPUID leaf 7, subleaf 0, EBX bit 20 = SMAP.
     * cpuid clobbers EAX, EBX, ECX, EDX; declare all four. */
    __asm__ volatile (
        "cpuid"
        : "=b"(ebx)
        : "a"(7), "c"(0)
        : "eax", "ecx", "edx"
    );
    return (ebx >> 20) & 1;
}

void
arch_smap_init(void)
{
    if (!cpuid_smap_supported()) {
        printk("[SMAP] WARN: not supported by CPU\n");
        return;
    }
    /* Set CR4.SMAP (bit 21 = 0x200000) */
    __asm__ volatile (
        "mov %%cr4, %%rax\n"
        "or $0x200000, %%rax\n"
        "mov %%rax, %%cr4\n"
        : : : "rax"
    );
    printk("[SMAP] OK: supervisor access prevention active\n");
}
```

### Additions to `kernel/arch/x86_64/arch.h`

Declaration (add after the SYSCALL section):
```c
/* arch_smap_init — detect SMAP via CPUID and enable CR4.SMAP if supported.
 * Prints [SMAP] OK or [SMAP] WARN. Must be called after arch_syscall_init(). */
void arch_smap_init(void);
```

Static inline macros (add immediately after the declaration):
```c
/* arch_stac — set RFLAGS.AC, temporarily permitting ring-0 access to
 * user-mode pages under SMAP. Bracket ONLY the single instruction that
 * loads from a user address; always pair with arch_clac() and never call
 * any function between stac and clac. No-op if SMAP is not enabled.
 * The "memory" clobber prevents the compiler from hoisting user-memory
 * loads before stac. */
static inline void arch_stac(void) { __asm__ volatile("stac" ::: "memory"); }

/* arch_clac — clear RFLAGS.AC, re-enabling SMAP protection.
 * Must be called after every arch_stac(). The "memory" clobber prevents
 * the compiler from sinking user-memory loads past clac. */
static inline void arch_clac(void) { __asm__ volatile("clac" ::: "memory"); }
```

### `kernel/syscall/syscall.c`

Add `#include "arch.h"` to the include block (required for `arch_stac`/`arch_clac`).

Add `USER_ADDR_MAX` and `user_ptr_valid` above `sys_write`:

```c
/* USER_ADDR_MAX — highest canonical user-space virtual address (x86-64).
 * Phase 9 forward-looking: move to arch.h when a second syscall file needs it. */
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFUL

/* user_ptr_valid — return 1 if [addr, addr+len) lies entirely within the
 * canonical user address space, 0 otherwise.
 * For len=0, validates that addr itself is a canonical user address (does
 * NOT unconditionally pass — a kernel addr with len=0 still returns 0).
 * Overflow-safe: addr <= USER_ADDR_MAX - len avoids addr+len wraparound. */
static inline int
user_ptr_valid(uint64_t addr, uint64_t len)
{
    return len <= USER_ADDR_MAX && addr <= USER_ADDR_MAX - len;
}
```

Updated `sys_write` — narrow `stac`/`clac` window to the single byte load:

```c
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;   /* EFAULT */
    const char *s = (const char *)(uintptr_t)arg2;
    uint64_t i;
    for (i = 0; i < arg3; i++) {
        char c;
        /* Narrow stac/clac window: bracket only the single user-memory load.
         * Never call functions between stac and clac.
         * Per-character stac/clac is intentionally conservative — IF=0
         * throughout syscall dispatch so there is no interrupt exposure risk.
         * Phase 9: replace with copy_from_user pattern (stac, memcpy to
         * kernel scratch buffer, clac, then pass kernel buffer to printk)
         * once a kernel scratch allocator exists. */
        arch_stac();
        c = s[i];
        arch_clac();
        printk("%c", c);
    }
    return arg3;
}
```

The `(const char *)(uintptr_t)arg2` cast is safe: `user_ptr_valid` has
confirmed `arg2` is a canonical user-space address, and `stac` permits the
load before `clac` restores protection.

### `kernel/core/main.c`

Call `arch_smap_init()` immediately after `arch_syscall_init()`:

```c
    arch_syscall_init();        /* SYSCALL/SYSRET — [SYSCALL] OK             */
    arch_smap_init();           /* SMAP detect + enable — [SMAP] OK/WARN     */
```

### `Makefile`

Add `arch_smap.c` to `ARCH_SRCS`:
```makefile
ARCH_SRCS = \
    ...
    kernel/arch/x86_64/arch_smap.c
```

### `tests/run_tests.sh`

Add `-cpu Broadwell` to the `qemu-system-x86_64` invocation so SMAP is always
present and the boot.txt oracle is deterministic across QEMU versions:

```bash
timeout 10s qemu-system-x86_64 \
    -machine pc \
    -cpu Broadwell \
    -cdrom "$ISO" \
    ...
```

**Note:** `make run` (interactive) does not use `run_tests.sh` and retains the
host CPU. On hosts without SMAP, `make run` will emit `[SMAP] WARN` instead
of `[SMAP] OK`. This is expected behavior — the test oracle is enforced only
by `make test` (which uses `-cpu Broadwell`).

**Implementer note:** Before committing `tests/expected/boot.txt` with the
`[SMAP] OK` line, run `make test` once and verify the actual output contains
`[SMAP] OK` (not `[SMAP] WARN`). QEMU's Broadwell CPU model exposes SMAP
(`CPUID.7.0:EBX[20]=1`), but this should be confirmed empirically against
the installed QEMU version rather than assumed.

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/arch/x86_64/arch_smap.c` | New — CPUID check + CR4.SMAP enable |
| `kernel/arch/x86_64/arch.h` | Add `arch_smap_init` declaration; add `arch_stac`/`arch_clac` static inlines |
| `kernel/syscall/syscall.c` | Add `#include "arch.h"`; add `USER_ADDR_MAX` + `user_ptr_valid`; EFAULT return; narrow `stac`/`clac` around single byte load |
| `kernel/core/main.c` | Call `arch_smap_init()` after `arch_syscall_init()` |
| `Makefile` | Add `arch_smap.c` to `ARCH_SRCS` |
| `tests/run_tests.sh` | Add `-cpu Broadwell` to QEMU invocation |
| `tests/expected/boot.txt` | Add `[SMAP] OK: supervisor access prevention active` after `[SYSCALL] OK` |
| `.claude/CLAUDE.md` | Update build status table |

---

## Test Oracle

`tests/expected/boot.txt` gains one line after `[SYSCALL] OK`:

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[CAP] OK: capability subsystem reserved
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SMAP] OK: supervisor access prevention active
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 3 tasks
[USER] hello from ring 3
[USER] hello from ring 3
[USER] hello from ring 3
[USER] done
[AEGIS] System halted.
```

---

## Success Criteria

1. `make test` exits 0.
2. `grep -rn 'arch_stac()\|arch_clac()' kernel/` shows exactly two call sites
   in `syscall.c` (one `arch_stac()`, one `arch_clac()`). Using the full call
   syntax avoids matching declarations and doc comments in `arch.h`.
3. `user_ptr_valid(0xFFFFFFFF80000000UL, 1)` returns 0 (kernel address
   rejected); `user_ptr_valid(0x400000UL, 4096)` returns 1 (valid user range).

---

## Phase 9 Forward-Looking Constraints

**`USER_ADDR_MAX` should move to `arch.h`.** Currently defined in
`syscall.c`. When `sys_read` or any other pointer-taking syscall is added in
Phase 9, copy-pasting the constant is a vulnerability. Move it to `arch.h`
(or a new `kernel/arch/x86_64/arch_uabi.h` included by `arch.h`) so all
syscall handlers share a single definition.

**`user_ptr_valid` should move to a shared header.** Same reason — a
`kernel/syscall/syscall_util.h` with `user_ptr_valid` as a static inline
ensures every future syscall gets validation for free.

**`make run` WARN behavior.** On hosts without SMAP, `make run` emits
`[SMAP] WARN`. This is not a bug, but it can confuse developers who diff
against `boot.txt` manually. A Phase 9 improvement would be to make the
`run` target also use `-cpu Broadwell`, or document this explicitly in the
Makefile comment.
