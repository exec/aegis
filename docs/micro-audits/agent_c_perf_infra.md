# Agent C: Performance and Infrastructure Micro-Audits

Date: 2026-04-01

---

## C1. Fork Page Copy Hot Path

**Severity: HIGH**

**Affected files:**
- `kernel/syscall/sys_process.c:473-690` (sys_fork)
- `kernel/mm/vmm.c:748-835` (vmm_copy_user_pages)

### Findings

**Page count estimate for a typical fork:**
A Lumen session terminal forks the shell (oksh/stsh). The forking process (the shell) has approximately:
- ELF text + data segments: ~20-50 pages
- libc.so loaded via PT_INTERP: ~200-400 pages (musl dynamic)
- Heap (brk region): ~10-50 pages
- mmap regions (thread stacks, anon): varies
- User stack: ~2-4 pages

A conservative estimate is 250-500 pages per fork. The comment at the top of `terminal.c` line 5 mentions "lumen's ~3000 pages" for the compositor process itself, which is why sys_spawn was introduced. For shell children, the typical count is lower but still significant.

**MMIO skip (VMM_FLAG_WC/UC):** Correctly implemented at `vmm.c:793`. The check `if (pte & (VMM_FLAG_WC | VMM_FLAG_UCMINUS)) continue;` skips framebuffer and device MMIO pages. VMM_FLAG_WC is bit 3 (PWT), VMM_FLAG_UCMINUS is bit 4 (PCD), defined in `vmm.h:28-29`. This is correct and prevents the multi-second stall from memcpy on MMIO ranges.

**Batch yield:** Implemented at `vmm.c:825-829`. Every 32 pages (`FORK_BATCH_SIZE = 32`), the vmm_window_lock is dropped and re-acquired:
```c
if (++batch >= FORK_BATCH_SIZE) {
    batch = 0;
    spin_unlock_irqrestore(&vmm_window_lock, fl);
    fl = spin_lock_irqsave(&vmm_window_lock);
}
```
This works but has a subtle issue: the lock drop/reacquire allows interrupt handlers to fire (IRQ restore), which enables PIT preemption and keeps the system responsive. However, the batch counter `batch` is only incremented for *copied* pages, not for skipped pages. The four-level page table walk itself is not yielded. If a process has many sparse page tables (many PML4/PDPT/PD entries with few actual leaf pages), the walk overhead between batch yields can be large.

**What COW fork would save:**
- Eliminates the entire `vmm_copy_user_pages` call (250-3000 page copies at ~4us/page = 1-12ms best case, seconds on bare metal with vmm_window_lock contention).
- Reduces fork to: duplicate page table structure with PTEs marked read-only, increment per-page refcount. Actual copy happens on first write (page fault handler).
- For fork+exec patterns (the shell's primary use), most pages are never written before exec replaces them. COW turns O(n_pages) fork into O(n_page_tables) + O(1) per subsequent write.
- Estimated savings: 95%+ of fork time in fork+exec patterns. The `terminal.c` comment confirms this was the motivation for sys_spawn.

### Recommended fix

1. Short-term: sys_spawn already mitigates the worst case (Lumen terminal creation). No immediate action needed for the shell fork+exec path at current scale.
2. Medium-term: Implement COW fork. Mark parent PTEs read-only on fork, track refcounts per physical page (add a `uint8_t refcount[]` array in PMM or use a separate bitmap). On write fault from ring-3, copy the faulting page and mark it writable. This is the single highest-impact performance improvement possible for the kernel.

---

## C2. Compositor Dirty-Rect Leak

**Severity: MEDIUM**

**Affected files:**
- `user/lumen/compositor.c:131-153` (comp_add_dirty)
- `user/lumen/compositor.c:284-426` (comp_composite)
- `user/lumen/compositor.h:10` (MAX_DIRTY_RECTS = 32)
- `user/glyph/window.c:369-391` (glyph_window_mark_dirty_rect / mark_all_dirty)
- `user/lumen/terminal.c:76-85` (term_scroll)

### Findings

**Dirty rect accumulation is bounded.** The compositor has a fixed-size array of 32 rects (`MAX_DIRTY_RECTS = 32` at `compositor.h:10`). When full, overflow rects are unioned into the last slot (`compositor.c:149-152`). This means dirty rect count is capped at 32 -- there is no unbounded growth.

At the end of every composite cycle, `c->ndirty` is reset to 0 (`compositor.c:424`). For full redraws, it is also reset at line 347. The accumulator does not leak across frames.

**Per-window dirty tracking is also bounded.** `glyph_window_t` has a single `dirty_rect` (a bounding box union) and a boolean `has_dirty` flag (`window.c:369-379`). Calling `mark_dirty_rect` unions into the existing rect; it never allocates. `glyph_window_render` clears `has_dirty` and zeros the rect (`window.c:246-248`).

**Terminal scroll does not allocate memory.** `term_scroll` at `terminal.c:76-85` advances the `scroll_top` index in a pre-allocated ring buffer of size `total_rows * cols` (allocated once at creation via `calloc` at line 733). Scrolling only modifies the index and clears a row via `memset`. No dynamic allocation occurs during scroll.

**The composite pass is O(dirty_rect_count) + O(n_windows).** The dirty rects are unioned into one bounding rect at `compositor.c:357-359`, making the actual redraw O(n_windows) (only windows overlapping the bounding rect are re-rendered). This is not the cause of the ~60s freeze.

**Actual suspected cause of Lumen freeze:** The freeze is more likely caused by:
1. **Frosted glass blur (draw_box_blur) per frame.** Each frosted window triggers a box blur over its entire area every frame it overlaps a dirty region (`compositor.c:182,194`). At 1080p with 2+ frosted windows, this is millions of pixel reads per frame.
2. **render_chrome per-pixel rounded rectangle test.** `window.c:150-172` iterates every pixel in the titlebar and client area, calling `outside_rounded()` per pixel (4 distance checks). For a 700x500 window, that is ~350K distance calculations per render.
3. **No incremental chrome rendering.** `glyph_window_render` always calls `render_chrome` first (`window.c:230`), which fills the entire window surface. Even when only a few terminal characters changed, the full chrome is redrawn.
4. **Memory pressure accumulation.** Each terminal allocates `(rows + 500) * cols` bytes for the grid plus a full `surf_w * surf_h * 4` byte surface buffer. Multiple terminals could push toward OOM.

### Recommended fix

1. **Cache chrome rendering.** Only re-render chrome on focus change, title change, or resize. Currently `render_chrome` runs on every `glyph_window_render` call, even for terminal windows where only the client area changed.
2. **Skip blur for non-overlapping dirty regions.** If the dirty rect does not intersect a frosted window's area, skip the blur entirely.
3. **Precompute rounded rect masks.** Replace per-pixel `outside_rounded()` calls with a precomputed 1-bit mask or edge table. This alone would cut chrome rendering time by ~10x.
4. Dirty rect count itself is NOT the issue -- the bound of 32 with union overflow is correct.

---

## C3. ACPI Deep Dive

**Severity: MEDIUM (one HIGH sub-finding)**

**Affected files:**
- `kernel/arch/x86_64/acpi.c:143-224` (scan_dsdt_s5, parse_fadt)
- `kernel/arch/x86_64/acpi.c:457-512` (acpi_power_button_init)
- `kernel/arch/x86_64/acpi.c:514-534` (acpi_do_poweroff)
- `kernel/arch/x86_64/acpi.c:536-554` (acpi_sci_handler)

### (a) FADT field offsets

Checking against ACPI 6.5 Specification, Table 5.9 (FADT):

| Field | CLAUDE.md offset | Code offset | ACPI spec offset | Verdict |
|-------|-----------------|-------------|------------------|---------|
| SCI_INT | 46 | 46 (line 196) | 46 (2 bytes) | CORRECT |
| SMI_CMD | 48 | 48 (line 197) | 48 (4 bytes) | CORRECT |
| ACPI_ENABLE | 52 | 52 (line 198) | 52 (1 byte) | CORRECT |
| PM1a_EVT_BLK | 56 | 56 (line 199) | 56 (4 bytes) | CORRECT |
| PM1a_CNT_BLK | 64 | 64 (line 201) | 64 (4 bytes) | CORRECT |
| PM1b_CNT_BLK | 68 | 68 (line 200) | 68 (4 bytes) | CORRECT |
| PM1_EVT_LEN | 88 | 88 (line 202) | 88 (1 byte) | CORRECT |

All offsets are correct per the ACPI specification. The CLAUDE.md documentation matches the code.

**Note:** `SCI_INT` at offset 46 is a `uint16_t` in the spec, but the code reads it as `(uint16_t)phys_read32(hdr_phys + 46)`. This reads 4 bytes starting at offset 46, which overlaps into SMI_CMD at offset 48. The cast to `uint16_t` truncates correctly to the lower 2 bytes (little-endian), so the value is correct. However, reading 2 extra bytes past the intended field is technically an overread. Harmless in practice since all fields are within the FADT.

### (b) DSDT _S5_ scan robustness

**HIGH sub-finding.** The `scan_dsdt_s5` function (`acpi.c:143-186`) scans raw DSDT bytecode for the literal `_S5_` string. Two robustness issues:

1. **False positive in string or method body.** The scan checks that the preceding byte is `NameOp (0x08)` or `\NameOp (0x5C 0x08)` at line 163. This is a good heuristic that eliminates most false matches (strings in DSDT buffers, method names referenced but not defined). However, a DSDT could contain a method that constructs the string "_S5_" as data bytes followed by a coincidental 0x08 byte. This is unlikely but not impossible.

2. **PkgLength parsing is fragile.** At line 168, the code parses AML PkgLength:
   ```c
   uint8_t pkg_lead = phys_read8(p);
   p += (uint64_t)(pkg_lead >> 6) + 1;
   ```
   This uses the top 2 bits to determine the byte count (0=1, 1=2, 2=3, 3=4), which matches the AML spec (ACPI 6.5 section 20.2.4). This is correct.

3. **Integer encoding limited.** Lines 172-178 only handle `BytePrefixOp (0x0A)` and direct values 0-1. If a DSDT encodes SLP_TYP as a `WordPrefixOp (0x0B)` or `DWordPrefixOp (0x0C)`, the parser silently reads the wrong value. In practice, S5 sleep types are small integers (0-7), so this works on all tested platforms. But it could fail on exotic firmware.

### (c) SCI handler status clearing

The SCI handler (`acpi.c:536-554`) is correct:
- Reads PM1_STS (`inw_port(evt_base)`) at line 544
- Reads PM1_EN (`inw_port(evt_base + en_off)`) at line 545
- Checks both PWRBTN_STS (bit 8) AND PWRBTN_EN (bit 8) before acting (line 547)
- Clears PWRBTN_STS by writing 1 to bit 8 (write-1-to-clear) at line 549

This is correct per ACPI spec. Non-power-button SCI events (thermal, GPE, etc.) are silently ignored, which is fine since we only enable PWRBTN_EN.

### (d) UEFI mode check

The ACPI mode transition at `acpi.c:466-482` correctly checks `SCI_EN` (bit 0 of PM1a_CNT) before attempting the SMI_CMD write:
```c
uint16_t pm1_cnt = inw_port((uint16_t)s_pm1a_cnt);
if (!(pm1_cnt & 1)) {  /* SCI_EN not set -> legacy mode */
```
On UEFI-booted systems, `SCI_EN` is already set (firmware boots in ACPI mode), so the `outb_port(s_smi_cmd, s_acpi_enable)` is never executed. This is correct.

### (e) ThinkPad X13 Zen 2 black screen on SMI_CMD write

The SMI_CMD write transitions the chipset from legacy to ACPI mode. Potential causes of a black screen:
1. **SMI handler reconfigures VGA.** The SMI handler in firmware (stored in SMRAM) may reprogram display hardware during the legacy-to-ACPI transition. On ThinkPad X13, the AMD GPU may lose its framebuffer configuration.
2. **Timeout is too short.** The 3000-iteration polling loop (`acpi.c:472-476`) with `inb(0x80)` delays is ~3ms. Some chipsets need up to 100ms for the SMI to complete. If the transition times out, `SCI_EN` may not be set yet.
3. **Already in ACPI mode.** If booted via UEFI (the normal case for ThinkPad X13), this code path is never hit. A black screen on ThinkPad X13 is likely unrelated to SMI_CMD -- it is more probably a framebuffer reconfiguration issue during IOAPIC/SMP init.

### Recommended fixes

1. For the `_S5_` scanner: Add a fallback default `SLP_TYPa = 5` (the value used by QEMU's DSDT) if the scan finds no match. Most real DSDT tables produce a match, but a default prevents a bricked power button.
2. Handle `WordPrefixOp (0x0B)` in the SLP_TYP parser for completeness.
3. Increase the SMI_CMD timeout from 3000 to 30000 iterations for conservative hardware compatibility.

---

## C4. Test Coverage Gaps

**Severity: HIGH**

**Affected files:** `tests/run_tests.sh`, `tests/expected/boot.txt`, `tests/test_*.py`

### Coverage matrix

| Subsystem | Boot oracle | Dedicated test_*.py | Status |
|-----------|:-----------:|:-------------------:|--------|
| Serial/VGA/printk | Yes | No | Implicitly tested by all other tests |
| PMM/VMM/KVA | Yes | No | Implicitly tested by fork/exec |
| IDT/PIC/PIT | Yes | No | Implicitly tested by scheduler |
| PS/2 keyboard | Yes | No | Implicitly tested by shell tests |
| Scheduler | Yes | No | Implicitly tested by fork/waitpid |
| GDT/TSS/SYSCALL | Yes | No | Implicitly tested by all user tests |
| SMAP/SMEP | Yes | No | **No adversarial test** |
| ELF loader | Yes | No | Implicitly tested by process spawn |
| Capabilities (Rust) | Yes | No | Implicitly tested by VFS gating |
| VFS + initrd | Yes | test_integrated | Covered |
| Shell + musl | Yes | test_integrated | Covered |
| Pipes + redirection | No | test_pipe | Covered |
| Signals | No | test_signal | Covered |
| stat/getdents64 | No | test_stat | Covered |
| PCIe + ACPI | Yes | No | **No test for power button** |
| NVMe | No | test_nvme | Covered |
| ext2 | No | test_ext2 | Covered |
| xHCI + USB HID kbd | No | test_xhci | Covered |
| USB HID mouse | No | test_mouse | Covered |
| GPT partitions | No | test_gpt | Covered |
| virtio-net | No | test_virtio_net | Covered |
| Net stack | No | test_net_stack | Covered |
| Socket API | No | test_socket | Covered |
| DHCP | Yes (via vigil) | test_vigil (indirect) | Partially covered |
| Threads (clone/futex) | No | test_threads | Covered |
| mprotect/mmap freelist | No | test_mmap | Covered |
| /proc filesystem | No | test_proc | Covered |
| TTY/PTY | No | test_pty | Covered |
| Dynamic linking | No | test_dynlink | Covered |
| Installer | No | test_installer | Covered |
| Symlinks + chmod | No | test_symlink | Covered |
| stsh shell | No | test_stsh | Covered |
| Lumen compositor | No | test_lumen | **Unknown coverage** |
| Bastion (display mgr) | No | No | **No test** |
| capd | No | No (capd_test binary exists but not in run_tests.sh) | **Partially tested** |
| SMP | Yes | No | **No concurrent stress test** |
| IPC (AF_UNIX/memfd) | No | No (ipc_test binary exists but not in run_tests.sh) | **Missing from run_tests.sh** |
| ACPI power button | No | No | **No test** |
| ext2 write path | No | test_ext2 (partial) | Persistence tested |
| Fork/exec (sys_fork) | No | No (implicit in shell tests) | **No stress test** |
| sys_spawn | No | No | **No dedicated test** |

### Three highest-risk untested paths

1. **ACPI power button / S5 shutdown (CRITICAL gap).** `acpi_do_poweroff` writes directly to PM1a_CNT to initiate hardware power off. There is no test that verifies S5 shutdown works, no test for the SCI handler, no test for the DSDT _S5_ scan producing a correct value. A bug here could brick bare-metal power off or, worse, corrupt the ext2 filesystem if `ext2_sync()` fails silently.

2. **IPC subsystem (ipc_test binary not wired into run_tests.sh).** `user/ipc_test/ipc_test.elf` exists and the Build Status table says "ipc_test 5/5", but `tests/run_tests.sh` does not invoke a `test_ipc.py`. If the ipc_test binary is only run manually, regressions in AF_UNIX, SCM_RIGHTS, or memfd_create could go undetected. Since capd depends on AF_UNIX, this is a transitive risk to the entire capability delegation system.

3. **SMP correctness under load (no concurrent stress test).** The boot oracle confirms SMP boots, but there is no test that runs multiple user processes concurrently on multiple CPUs. The ~30 spinlocks in the kernel have documented ordering constraints but no automated deadlock detection. A concurrent fork+exec stress test could expose lock ordering violations, TLB shootdown races, or per-CPU state corruption.

### Recommended fixes

1. Add `test_ipc.py` to `run_tests.sh` (or wire the existing ipc_test binary into test_integrated).
2. Add `test_capd.py` to `run_tests.sh` (capd_test binary already exists).
3. Create a `test_acpi_shutdown.py` that verifies `[ACPI] initiating S5 power off` appears on the serial before QEMU exits.
4. Create a concurrent stress test: fork 8+ processes that all fork+exec in parallel, verify all complete via waitpid.

---

## C5. Scheduler O(n) Scan + Sleep Inefficiency

**Severity: MEDIUM**

**Affected files:**
- `kernel/sched/sched.c:492-572` (sched_tick)
- `kernel/sched/sched.c:289-346` (sched_block)
- `kernel/sched/sched.c:415-451` (sched_yield_to_next)
- `kernel/sched/sched.c:349-358` (sched_wake)

### Findings

**sched_tick walks all tasks twice:**

1. **Sleep-wake scan (lines 505-517):** Iterates the entire circular task list to check `sleep_deadline` against current ticks. Every TASK_BLOCKED task with a non-zero sleep_deadline is examined. This is O(n_total_tasks).

2. **Next-runnable scan (lines 521-524):** Starting from `old->next`, walks the circular list until it finds a TASK_RUNNING task. This is O(n_tasks) worst case (if the only runnable task is just before `old` in the list).

Both scans run at 100 Hz (PIT frequency), meaning 200 * n_tasks pointer chases per second.

**Task count in a typical Lumen session:**
- `task_idle` (kernel, always RUNNING)
- `vigil` (init, often BLOCKED on waitpid)
- `bastion` (display manager, often BLOCKED)
- `lumen` (compositor, RUNNING -- polling loop with nanosleep)
- `login` (BLOCKED on TTY read or exited)
- `dhcp` (BLOCKED on socket or exited)
- `capd` (BLOCKED on accept)
- `httpd` (BLOCKED on accept)
- Per terminal: `sh` (BLOCKED on read), potentially child processes (BLOCKED or RUNNING)

Estimate: 8-15 tasks total, of which 2-3 are RUNNING at any given time. The remaining 5-12 are BLOCKED or ZOMBIE.

**Fraction blocked:** ~70-85% of tasks are BLOCKED. The scheduler wastes most of its 100 Hz scan time touching tasks that cannot possibly be scheduled.

**sched_block also scans O(n):** At line 312-314, `sched_block` walks the list to find the next RUNNING task. Same O(n) cost.

**BLOCKED tasks remain in the circular list** (explicitly documented at line 297-308). The comment explains this is intentional: `sched_exit`'s zombie-wakeup scan traverses the run queue looking for blocked parents. Removing tasks from the queue would break this scan.

### Impact assessment

At 8-15 tasks, the O(n) scan is negligible -- a few hundred nanoseconds per tick. The scheduler is not the cause of the Lumen freeze. However, this design does not scale:
- At 50 tasks: ~10us per tick (still fine)
- At 200 tasks (server workload with many connections): ~40us per tick at 100 Hz = 0.4% CPU overhead
- At 1000 tasks: measurable scheduler overhead

The sleep-deadline scan is the more egregious issue: it checks every task's deadline even when no tasks are sleeping.

### Recommended fix

**Separate RUNNING list (medium effort, high payoff at scale):**
1. Maintain a doubly-linked `runqueue` containing only TASK_RUNNING tasks.
2. `sched_block` removes the task from the runqueue; `sched_wake` inserts it back.
3. `sched_tick`'s next-task scan becomes O(n_running) instead of O(n_total).
4. The `sched_exit` zombie-wakeup scan needs adjustment: either keep a separate `all_tasks` list for zombie parent lookups, or change the wakeup mechanism to use direct parent pointers.

**Timer wheel for sleep deadlines (lower priority):**
Replace the per-tick full scan with a sorted list or timer wheel. `sys_nanosleep` inserts into the wheel; the tick handler only checks the head entry. O(1) per tick instead of O(n).

**Short-term: no action needed.** At current task counts (8-15), the O(n) scan adds <1us per tick. Fix when task count exceeds ~50 or when the server workload (SSH, multi-connection httpd) is implemented.
