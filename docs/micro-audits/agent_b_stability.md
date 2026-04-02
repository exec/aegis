# Agent B — Stability & Resource Exhaustion Micro-Audits

Date: 2026-04-01
Auditor: Claude Opus 4.6 (read-only, no code changes)

---

## B1. SMP Data Races Catalog

The kernel boots APs but only BSP runs user tasks. However, ISRs fire on
all cores, and the following structures are accessed from syscall context
without locks. On SMP these are latent data races even with the current
"only BSP runs user tasks" model if an ISR on an AP touches shared state.

### B1.1 mmap freelist (sys_memory.c)

**Severity: HIGH**
**Files:** `kernel/syscall/sys_memory.c:106-169`

`mmap_free_insert()` and `mmap_free_alloc()` operate on per-process
`proc->mmap_free[]` and `proc->mmap_free_count` without any lock.
Currently safe because:
- Syscalls are non-preemptible on a single core.
- CLONE_VM threads share the address space but run sequentially.

**Race (SMP):** Two threads in the same CLONE_VM group call `mmap`/`munmap`
concurrently on different CPUs. Both read `mmap_free_count`, both insert at
the same index, one entry is silently lost. Or `mmap_free_alloc` reads a
partially-written `mmap_free_t` entry.

**Fix:** Add a per-process `spinlock_t mmap_lock` to `aegis_process_t`.
Acquire in `sys_mmap`, `sys_munmap`, and `sys_mprotect` around freelist
and VMA operations.

### B1.2 fd_table (fd_table.c)

**Severity: HIGH**
**Files:** `kernel/fs/fd_table.c:1-53`

`fd_table_t` has atomic refcounting (`__atomic_fetch_add/sub` at lines
18-26) but the fd array `fds[]` itself has no lock. `fd_table_copy` at
line 38 does a plain memcpy of all 32 fd slots without holding any lock.

**Race (SMP):** Thread A calls `close(fd)` while Thread B (same
CLONE_FILES group) calls `read(fd)`. Thread B reads `fds[fd].ops` as
non-NULL, then Thread A sets `fds[fd].ops = NULL` and calls `close()`.
Thread B dereferences a freed `priv` pointer. This is a use-after-free.

**Fix:** Add a `spinlock_t lock` field to `fd_table_t`. Acquire around
every `fds[]` read/write in syscall dispatch (sys_read, sys_write,
sys_close, sys_dup, sys_dup2, sys_pipe2).

### B1.3 VMA table (vma.c)

**Severity: HIGH**
**Files:** `kernel/mm/vma.c:36-320`

`vma_insert`, `vma_remove`, `vma_update_prot` directly manipulate
`proc->vma_table[]` and `proc->vma_count` without locks. `vma_share`
(line 302) shares the same table pointer between parent and child with a
plain increment of `vma_refcount` (no atomic).

**Race (SMP):** Two CLONE_VM threads concurrently call `mmap` and
`munmap`. Both call `vma_insert`/`vma_remove` which shift array elements.
The array becomes internally inconsistent (overlapping entries, lost
entries, corrupted count).

Additionally, `vma_share` at line 307 does
`child->vma_refcount = parent->vma_refcount` which is not atomic. If the
parent frees concurrently, the refcount is corrupted.

**Fix:** Protect with the same per-process `mmap_lock` recommended in
B1.1. Make `vma_refcount` operations atomic.

### B1.4 Futex waiters (futex.c)

**Severity: MEDIUM**
**Files:** `kernel/syscall/futex.c:1-79`

The futex pool is properly protected by `futex_lock` (spinlock with IRQ
save). However, the FUTEX_WAIT path has a subtle TOCTOU: `copy_from_user`
at line 48 reads the futex word, then at line 53 acquires `futex_lock` and
blocks. Between the read and the lock acquisition, another thread could
change the value AND call FUTEX_WAKE. The wake sees no waiter (not yet
registered), the waiter registers and blocks forever.

**Race:** Thread A reads `*addr == val` at line 50. Thread B writes
`*addr = new_val` and calls `futex(FUTEX_WAKE)` between lines 50 and 66.
Wake finds no waiter. Thread A registers itself and blocks at line 70.
Permanent hang.

**Fix:** Read the futex word under `futex_lock` (map user page, read
under lock). This matches Linux's approach where the futex hash bucket
lock is held during the compare-and-queue operation.

### B1.5 Summary table

| Structure | Lock | Race condition | Severity |
|-----------|------|----------------|----------|
| mmap freelist | None | Concurrent mmap/munmap corrupts freelist | HIGH |
| fd_table fds[] | None (refcount only) | Use-after-free on concurrent close+read | HIGH |
| VMA table | None | Array corruption on concurrent insert/remove | HIGH |
| Futex pool | `futex_lock` (good) | TOCTOU between userspace read and lock | MEDIUM |
| Pipe ring | `pipe_t.lock` (good) | Properly locked | OK |
| PTY pair | `pair->lock` (partial) | Ring buffer ops not locked (see B4) | MEDIUM |

---

## B2. Memory Leak per fork/exec/exit Cycle

### B2.1 KVA bump allocator: does kva_free_pages reclaim VA?

**Severity: MEDIUM**
**Files:** `kernel/mm/kva.c:74-109, 164-179`

Yes, `kva_free_pages` does reclaim VA. It unmaps pages, frees physical
frames via `pmm_free_page`, then inserts the VA range into a 128-slot
freelist with coalescing (lines 76-108). `kva_alloc_pages` checks the
freelist before bumping `s_kva_next` (lines 117-122).

**Leak path:** If the freelist is full (128 entries), the freed VA range
is silently dropped (line 108: `/* else: freelist full, VA is leaked */`).
Each fork+exec+exit cycle allocates:
- 1 page: fd_table_t (fork creates copy, exec creates new, freed on exit)
- 1 page: vma_table (fork creates copy, freed on exit)
- 4 pages: kernel stack (freed on waitpid reap)
- 2 pages: PCB (freed on waitpid reap)

That is 8 pages allocated and 8 freed per cycle. The freelist has 128
slots. With coalescing, adjacent frees merge into single entries. In
practice, the freelist should not saturate under normal fork+exec+exit
patterns because allocations and frees interleave.

**Actual leak: 0 pages per clean cycle.** Physical memory is fully
reclaimed. VA is reclaimed unless the freelist overflows.

### B2.2 mmap pages after mprotect(PROT_NONE) + munmap

**Severity: HIGH**
**Files:** `kernel/syscall/sys_memory.c:354-391, 404-438`

This is a confirmed leak documented in Phase 30 constraints.
`sys_mprotect` with `PROT_NONE` clears the PRESENT bit (line 421:
`flags = 0`). Later, `sys_munmap` calls `vmm_phys_of_user` (line 377)
which walks the page tables looking for PRESENT entries. Since PRESENT is
cleared, `vmm_phys_of_user` returns 0, and `pmm_free_page` is never
called. The physical frame is permanently leaked.

**Impact per thread:** musl's `pthread_create` calls
`mprotect(guard_page, 4096, PROT_NONE)` for each thread stack guard page.
When the thread exits and the parent calls `munmap` on the stack region,
the guard page's physical frame (1 page = 4KB) is leaked.

**Quantification:** 1 page (4KB) leaked per thread creation+destruction
cycle. For a server spawning 100 threads: 400KB leaked permanently.

### B2.3 Pipe ring buffers

**Severity: LOW (correct)**
**Files:** `kernel/fs/pipe.c:197-232`

Pipe ring buffers are allocated as 1 kva page. Both
`pipe_read_close_fn` and `pipe_write_close_fn` check if both refcounts
are zero before calling `kva_free_pages`. This is correct. Refcounts are
incremented by `pipe_dup_read_fn`/`pipe_dup_write_fn` under the pipe's
spinlock. No leak.

### B2.4 Unix socket ring buffers

**Severity: MEDIUM**
**Files:** `kernel/net/unix_socket.c:189-249`

The deferred ring buffer cleanup is intentional but creates a semi-leak.
When side A closes, its ring buffer is NOT freed if side B is still alive
(line 241). Side B's eventual close frees both rings. However, if side B
never closes (leaked fd, stuck process), both ring buffers (2 kva pages =
8KB) are permanently leaked.

Additionally, `unix_sock_alloc` at line 164 frees orphaned rings from
previous connections on slot reuse. This is a best-effort cleanup that
only runs when the slot is reallocated. If the 32-slot pool never wraps
around to that slot, the orphaned ring stays allocated.

**Quantification per unix socket pair:** 0 pages leaked in the normal
connect-exchange-close pattern. Up to 2 pages (8KB) leaked if one side
is abandoned.

### B2.5 The "ls / OOM" bug (Phase 45)

**Severity: HIGH**
**Files:** Likely `kernel/mm/kva.c` or `kernel/syscall/sys_memory.c`

The `ls /` OOM noted since Phase 45 is most likely caused by the
cumulative effect of multiple small leaks rather than a single large one.
Candidate root causes:

1. **KVA freelist saturation:** Each capd connection creates a unix
   socket pair (2 ring pages). If capd handles many connections before
   slot reuse, the freelist fills up and VA ranges are lost. With a
   128-slot freelist and many small 1-page frees, fragmentation can
   prevent coalescing.

2. **PMM physical memory exhaustion:** Not a VA issue. `ls /` with a
   large rootfs may need brk + mmap pages. If prior processes leaked
   PROT_NONE guard pages (B2.2) or abandoned unix socket rings (B2.4),
   PMM free page count drops. With 128MB RAM, there is very little
   headroom.

3. **fd_table copy in fork:** Each `fork()` allocates 1 kva page for
   `fd_table_copy` (fd_table.c:41). If the child exec's, a NEW fd_table
   is allocated and the fork'd copy is freed. But the exec path also
   allocates a vma_table, kernel stack, etc. The peak allocation during
   fork+exec is ~10+ kva pages simultaneously before the old ones are
   freed.

**Recommended investigation:** Add `pmm_free_pages()` and `s_nfree`
printk at `ls` exec time to determine if it is physical or virtual
exhaustion.

### B2.6 Summary: pages leaked per fork+exec+exit cycle

| Resource | Allocated | Freed | Leaked | Condition |
|----------|-----------|-------|--------|-----------|
| PCB (2 pages) | fork | waitpid reap | 0 | Normal |
| Kernel stack (4 pages) | fork | waitpid reap | 0 | Normal |
| fd_table (1 page) | fork | exit (unref) | 0 | Normal |
| vma_table (1 page) | fork | waitpid reap | 0 | Normal |
| Thread guard page (1 page) | mprotect(PROT_NONE) | munmap | **1** | Always |
| Unix socket ring (1-2 pages) | connect | close both sides | 0-2 | If one side abandoned |

**Net: 0 pages leaked per simple fork+exec+exit. 1 page leaked per
thread create+destroy cycle (guard page). Up to 2 pages per abandoned
unix socket.**

---

## B3. ext2 Corruption Resilience

### B3.1 Write-back vs write-through

**Severity: MEDIUM**
**Files:** `kernel/fs/ext2_cache.c:1-125`

The block cache uses **write-back**: dirty blocks stay in the 16-slot LRU
cache and are only flushed in two scenarios:
1. **Eviction** (line 43-53): When the LRU slot is needed for a new
   block, the old dirty data is written to disk first.
2. **ext2_sync** (line 99-124): Explicitly flushes all 16 dirty slots.

There is no periodic sync. If the system crashes between writes to the
cache and eviction/sync, data is lost. The only call to `ext2_sync` is
from `sched_exit` when the last user process exits (sched.c:215) and from
the ACPI power button handler.

**Risk:** A power failure during normal operation (not clean shutdown)
loses all dirty cache contents. With only 16 slots, the window is small,
but metadata corruption (inode table, block bitmap, BGD) can render the
filesystem unmountable.

**Fix:** Add a periodic sync (e.g., every 30 seconds from a kernel timer
or PIT callback). Also flush on `ext2_write` for metadata blocks (bitmap,
inode table).

### B3.2 ext2_sync flush completeness

**Severity: MEDIUM**
**Files:** `kernel/fs/ext2_cache.c:99-124`, `kernel/fs/ext2.c:813-816`

`ext2_sync` flushes all 16 cache slots. However, the in-memory
superblock (`s_sb`) and block group descriptor table (`s_bgd[]`) are
modified by `ext2_alloc_block` (line 815: `s_sb.s_free_blocks_count--`)
and `ext2_alloc_inode` (line 841: `s_sb.s_free_inodes_count--`) but
**never written back to disk**. The superblock and BGD table are read at
mount time (lines 51-91) but there is no code path that writes them back.

**Impact:** After allocating blocks or inodes, the on-disk superblock and
BGD still show the old free counts. On next mount (after crash), `fsck`
would see inconsistent free counts. Worse, blocks/inodes marked as used
in the bitmap but with stale free counts in the superblock could be
double-allocated after a remount.

**Fix:** `ext2_sync` must write back `s_sb` to LBA 2 and `s_bgd[]` to
the BGD block after flushing cache slots.

### B3.3 Malformed inode crash potential

**Severity: HIGH**
**Files:** `kernel/fs/ext2.c:169-222, 230-288`

**(a) i_block pointing outside device:** `ext2_block_num` (line 169)
returns whatever block number is stored in `inode.i_block[]` or in
indirect block entries. The returned block number is passed directly to
`cache_get_slot`, which calls `s_dev->read(s_dev, lba, ...)`. The LBA is
computed as `block_num * (s_block_size / 512)`.

There is **no check** that `block_num < s_sb.s_blocks_count`. A malformed
inode with `i_block[0] = 0xFFFFFFFF` would compute
`lba = 0xFFFFFFFF * 2 = 0x1FFFFFFFE`. This is passed to the NVMe or
ramdisk read function. The NVMe driver likely returns an error (the LBA
exceeds the drive capacity), but the ramdisk driver at
`kernel/drivers/ramdisk.c` does a bounds check against `block_count`,
so it would return -1. `cache_get_slot` would then return NULL (line 86),
and the caller handles NULL. **No kernel crash, but read returns partial
data (0 bytes read).**

**(b) Huge indirect chains:** `ext2_block_num` supports up to double
indirect (line 195). Triple indirect returns 0 (line 221). The depth is
bounded. However, a circular indirect chain (block A points to block B
which points to block A) would cause `cache_get_slot` to ping-pong
between two cache slots indefinitely. The read loop in `ext2_read` would
eventually terminate because `bytes_read` advances and `len` is finite.
**No hang, but cache thrashing.**

**(c) i_size cap:** `ext2_read` rejects `i_size > 256MB` (line 243).
This is a reasonable defense. However, `ext2_write` has no such cap --
a malicious caller could write past 256MB by supplying large offsets.

**Fix:** Add `block_num < s_sb.s_blocks_count` check in
`cache_get_slot` or `ext2_block_num`. Add i_size cap to `ext2_write`.

### B3.4 Partition bounds enforcement

**Severity: MEDIUM**
**Files:** `kernel/fs/gpt.c:94-99`

`gpt_part_read` at line 97 does check `lba + count > dev->block_count`
and returns -1 if exceeded. This is correct -- partition bounds ARE
enforced at the blkdev layer.

However, ext2 itself does NOT check that block numbers fall within the
partition. It relies entirely on the blkdev bounds check. If the blkdev
`block_count` is correct, this is safe. If `block_count` were wrong
(e.g., from a malformed GPT entry where `end_lba < start_lba`), reads
could wrap around. The GPT code at line 238 computes
`end_lba - start_lba + 1` which would underflow if `end_lba < start_lba`.
GPT does not validate `start_lba <= end_lba`.

**Fix:** Add `start_lba <= end_lba` check in `gpt_scan`.

---

## B4. PTY/TTY Edge Cases

### B4.1 All 16 PTY slots used

**Severity: LOW**
**Files:** `kernel/tty/pty.c:411-418`

When all 16 slots are in use, `ptmx_open` returns `-12` (ENOMEM) at
line 418. The syscall layer propagates this as an error. Clean behavior.

However, there is no mechanism to wait for a slot to become available.
A compositor that spawns terminals rapidly could hit the limit and must
retry manually.

### B4.2 Master close -> EIO to slave

**Severity: LOW (correct)**
**Files:** `kernel/tty/pty.c:281-299, 116-139`

`master_close_fn` sets `pair->master_open = 0` (line 290) and wakes the
slave reader (line 294). The slave's `pty_slave_read_raw` checks
`!pair->master_open` at line 129 and returns -5 (EIO). Correct.

### B4.3 Use-after-free if slave reader blocked during master close

**Severity: MEDIUM**
**Files:** `kernel/tty/pty.c:121-139, 281-299`

`pty_slave_read_raw` blocks at line 137 (`sched_block()`). When master
closes, `master_close_fn` wakes the slave at line 294. The slave resumes,
re-enters the loop, checks `ring_count` (which may still be 0), then
checks `!pair->master_open` and returns EIO.

**Critical detail:** The `pty_pair_t` is statically allocated in
`s_pty_pool[16]` (BSS). It is never freed -- only `in_use` is cleared.
`memset` is called on the NEXT allocation (`ptmx_open` line 423). So
there is **no use-after-free** -- the slave reads from a stale but valid
memory location.

However, there is a **race between master close and slot reuse:**
1. Master closes, sets `master_open = 0`, slave_open already 0 -> `in_use = 0`
2. A NEW `ptmx_open` allocates the same slot, `memset`s it to zero
3. The old slave reader (still blocked, now woken) resumes in
   `pty_slave_read_raw`, reads `pair->master_open = 0` from the NEW
   pair's zeroed state, returns EIO.

This is benign (returns EIO either way) but is logically incorrect -- the
old slave is reading the new pair's state.

**Fix:** Check pair identity (e.g., generation counter) after waking from
`sched_block`.

### B4.4 SIGHUP on session leader exit

**Severity: LOW (correct with caveat)**
**Files:** `kernel/syscall/sys_process.c:47-54`

`sys_exit` sends SIGHUP + SIGCONT to the foreground process group when
the session leader exits (lines 48-54). This is correct per POSIX.

**Caveat:** Only the foreground group gets SIGHUP. Background process
groups in the same session do NOT receive SIGHUP. POSIX says orphaned
process groups with stopped members should receive SIGHUP + SIGCONT.
This is not implemented.

### B4.5 Orphaned process groups

**Severity: LOW**
**Files:** `kernel/syscall/sys_process.c:62-75`

Orphaned children are reparented to init (PID 1) at lines 63-74. This
is correct for zombie reaping. However, POSIX requires that when a
process group becomes orphaned (no member has a parent in a different
process group in the same session) AND has stopped members, the entire
group receives SIGHUP + SIGCONT. This is not implemented.

**Impact:** A background job stopped with Ctrl-Z, whose session leader
then exits, will remain stopped forever (no SIGHUP to wake it). The
process becomes a zombie only when manually killed.

### B4.6 PTY ring buffer operations lack per-pair lock

**Severity: MEDIUM**
**Files:** `kernel/tty/pty.c:86-105, 232-268`

`pty_slave_write_out` (line 86) and `master_write_fn` (line 237)
manipulate `input_buf`/`output_buf` ring buffers without acquiring
`pair->lock`. The lock is only used in `master_dup_fn`, `master_close_fn`,
`slave_dup_fn`, `slave_close_fn`, and pool allocation.

On single-core this is safe (syscalls are non-preemptible). On SMP, a
master write and slave read on different cores would race on ring head/tail
pointers without any synchronization.

**Fix:** Acquire `pair->lock` in all ring buffer operations
(`master_write_fn`, `master_read_fn`, `pty_slave_write_out`,
`pty_slave_read_raw`).

---

## B5. Pipe and Unix Socket Resource Exhaustion

### B5.1 Pipe full + no SIGPIPE handler

**Severity: LOW (correct)**
**Files:** `kernel/fs/pipe.c:129-189`

When the pipe is full (`p->count == PIPE_BUF_SIZE`), the writer blocks
at line 158 (`sched_block()`). This is correct -- the writer waits for
space.

When all readers are gone (`p->read_refs == 0`), `pipe_write_fn` sends
SIGPIPE to the writer (line 151) and returns -32 (EPIPE). If the process
has SIGPIPE masked or SIG_IGN, it receives EPIPE via errno. Correct
POSIX behavior.

**Edge case:** A kernel task (is_user == 0) writing to a pipe with no
reader gets -32 (EPIPE) without SIGPIPE (line 148 checks `t->is_user`).
Correct.

### B5.2 Unix socket slot exhaustion

**Severity: MEDIUM**
**Files:** `kernel/net/unix_socket.c:160-179, 301-397`

`UNIX_SOCK_MAX = 32`. Each connected pair uses 3 slots: 1 listener + 1
client + 1 server. A single listener with 8 pending connections in the
accept queue uses 1 + 8 = 9 slots minimum.

When all 32 slots are in use, `unix_sock_alloc` returns -1 (line 178).
`unix_sock_connect` also allocates a server-side socket internally (line
331); if this fails, it returns -24 (EMFILE).

**Exhaustion scenario:** 5 services each with a listener (5 slots) and
5 pending connections (25 slots) = 30 slots. Only 2 slots remain for
new connections. A burst of connect requests would fail with EMFILE/
ECONNREFUSED.

**Impact:** capd uses AF_UNIX. If the unix socket pool is exhausted,
capd cannot accept new connections, and service startup stalls. capd is
single-threaded and accepts connections sequentially, but a malicious
process could connect without sending data, holding a slot indefinitely.

**Fix:** Increase `UNIX_SOCK_MAX` to 64 or 128. Add a per-socket
connection timeout to reclaim stuck slots.

### B5.3 Deferred ring buffer cleanup correctness

**Severity: MEDIUM**
**Files:** `kernel/net/unix_socket.c:189-249, 160-179`

The deferred ring cleanup has a subtle issue. When side A closes (line
189), if side B is still alive, side A's ring is left in place (line 241).
Side A's `in_use` is set to 0 (line 247). The ring pointer remains
valid because `memset` is only called on slot reuse (line 169/338).

However, `unix_sock_alloc` at line 164 checks for orphaned rings:
```c
if (s_unix[i].ring) {
    kva_free_pages(s_unix[i].ring, 1);
    s_unix[i].ring = (void *)0;
}
```

This happens BEFORE `memset` at line 169. **Problem:** If side B has NOT
yet read all data from side A's ring, and a NEW connection reuses slot A,
the orphaned ring is freed (line 166) while side B may still be reading
from it. Side B accesses freed memory.

**Race scenario:**
1. A-B connected. A closes. `s_unix[A].in_use = 0`, ring still valid.
2. B is still alive, reading from A's ring at line 478.
3. New connection C calls `unix_sock_alloc`, finds slot A free.
4. Line 165: `kva_free_pages(s_unix[A].ring, 1)` -- frees the ring.
5. B reads from the now-freed ring page -- **use-after-free**.

**Severity: HIGH** (data corruption or kernel crash on SMP; on single-core
the sequential execution model prevents interleaving steps 2-4).

**Fix:** Do not free orphaned rings on slot reuse. Instead, side B must
detect that side A's ring is orphaned (via `!s_unix[peer].in_use`) and
free the ring after draining it. Or use a generation counter to detect
stale ring pointers.

### B5.4 Maximum kernel memory consumed

**Severity: LOW (informational)**

| Resource | Count | Per-unit | Total |
|----------|-------|----------|-------|
| Pipe ring buffers | Unbounded (1 per pipe2) | 1 kva page (4KB) | `n * 4KB` |
| Unix socket rings | Max 32 connections | 2 kva pages per pair (8KB) | 32 * 8KB = 256KB |
| Unix socket structs | 32 static | `sizeof(unix_sock_t)` in BSS | ~32KB BSS |
| PTY pairs | 16 static | `sizeof(pty_pair_t)` in BSS | ~138KB BSS |
| Futex waiters | 64 static | 24 bytes each in BSS | ~1.5KB BSS |

Pipe ring buffers are the only unbounded resource. Each `sys_pipe2` call
allocates 1 kva page. With `PROC_MAX_FDS = 32` per process and
`MAX_PROCESSES = 64`, the theoretical max is 64 * 16 pipe pairs = 1024
pipes = 4MB of kva pages. In practice, much less (shell pipelines
typically use 1-3 pipes).

**Risk:** A malicious process could call `pipe2` in a loop, allocating
kva pages until PMM exhaustion. There is no per-process pipe count limit.

**Fix:** Add a per-process pipe/fd limit check, or a global pipe count
cap.

### B5.5 Pipe single-waiter limitation

**Severity: LOW**
**Files:** `kernel/fs/pipe.c:78, 156`

`reader_waiting` and `writer_waiting` are single task pointers. If two
tasks read from the same pipe end (e.g., after `fork` + both children
inherit the read fd), only the LAST one to block is stored. The first
waiter is silently overwritten and never woken.

**Impact:** Rare in practice (shell pipelines have one reader per pipe
end). Could cause permanent hangs in concurrent-reader patterns.

**Fix:** Use a wait queue (linked list of waiters) instead of a single
pointer.

---

## Summary of Findings by Severity

| ID | Severity | Finding |
|----|----------|---------|
| B1.1 | HIGH | mmap freelist has no lock (SMP race) |
| B1.2 | HIGH | fd_table fds[] array has no lock (SMP use-after-free) |
| B1.3 | HIGH | VMA table has no lock (SMP corruption) |
| B2.2 | HIGH | mprotect(PROT_NONE) + munmap leaks physical frame |
| B3.3 | HIGH | ext2 block numbers not validated against s_blocks_count |
| B5.3 | HIGH | Unix socket ring freed while peer still reading (use-after-free on slot reuse) |
| B1.4 | MEDIUM | Futex TOCTOU between userspace read and registration |
| B2.4 | MEDIUM | Unix socket rings leaked if one side abandoned |
| B3.1 | MEDIUM | No periodic ext2 sync; data loss on crash |
| B3.2 | MEDIUM | ext2_sync does not write back superblock/BGD free counts |
| B3.4 | MEDIUM | GPT does not validate start_lba <= end_lba |
| B4.3 | MEDIUM | PTY slot reuse race (benign but incorrect) |
| B4.6 | MEDIUM | PTY ring buffers not locked for SMP |
| B5.2 | MEDIUM | Unix socket 32-slot pool easily exhausted |
| B2.5 | MEDIUM | "ls / OOM" likely PMM exhaustion from accumulated leaks |
| B4.4 | LOW | No SIGHUP to orphaned background process groups |
| B4.5 | LOW | POSIX orphaned process group handling missing |
| B4.1 | LOW | No PTY slot wait mechanism |
| B5.1 | LOW | Pipe full/SIGPIPE handling correct |
| B5.4 | LOW | Pipes are unbounded (no per-process limit) |
| B5.5 | LOW | Single-waiter pipe limitation |
