/* libinstall.h — Aegis installer library (Phase 2 extraction)
 *
 * Pure install logic shared between the text-mode installer and the
 * upcoming Glyph-based GUI installer.  All UI concerns (prompts,
 * progress display) live in the caller; libinstall emits progress
 * via callbacks and returns 0 / -1 on each operation.
 */
#ifndef LIBINSTALL_H
#define LIBINSTALL_H

#include <stdint.h>

/* Progress callback struct.  All callbacks are optional (NULL is OK).
 *
 *   on_step     — called at the start of each named phase
 *                 ("Writing GPT", "Copying rootfs", ...).
 *   on_progress — called while a phase is running, 0..100.
 *   on_error    — called with a human-readable message if a step fails.
 *                 libinstall always returns -1 from the failing call;
 *                 the error callback is purely for UI display.
 *
 * `ctx` is an opaque caller pointer passed back to every callback. */
typedef struct {
    void (*on_step)(const char *label, void *ctx);
    void (*on_progress)(int pct, void *ctx);
    void (*on_error)(const char *msg, void *ctx);
    void *ctx;
} install_progress_t;

/* Block device enumeration.  Layout matches the kernel syscall-510
 * ABI exactly: {char[16], uint64, uint32, uint32 pad}. */
typedef struct {
    char     name[16];
    uint64_t block_count;
    uint32_t block_size;
    uint32_t _pad;
} install_blkdev_t;

int install_list_blkdevs(install_blkdev_t *out, int max);

/* GPT — write protective MBR + primary GPT + backup GPT.
 * Creates 32 MB ESP (LBA 2048..67583) and Aegis root (rest of disk).
 * Returns 0 on success, -1 on I/O error (error callback fired). */
int install_write_gpt(const char *devname, uint64_t disk_blocks,
                      install_progress_t *p);

/* Ask the kernel to re-enumerate partitions on `devname`.
 * Returns the number of partitions found (>0 on success). */
int install_rescan_gpt(const char *devname);

/* Copy ramdisk1 (the ESP image embedded as module 2 in the live
 * ISO) to the ESP partition on `devname`. */
int install_copy_esp(const char *devname, install_progress_t *p);

/* Copy ramdisk0 (the ext2 rootfs image embedded as module 1) to
 * `dst_dev` (the Aegis root partition found via install_rescan_gpt).
 * Emits on_progress every 10%. */
int install_copy_rootfs(const char *dst_dev, uint64_t dst_blocks,
                        install_progress_t *p);

/* Write a fresh grub.cfg to /boot/grub/grub.cfg on the currently
 * mounted root (the ramdisk0 ext2 before it's copied to the target).
 * Must run before install_copy_rootfs. */
int install_write_grub_cfg(void);

/* Delete test binaries (thread_test, mmap_test, etc.) from the
 * currently mounted rootfs.  Must run before install_copy_rootfs. */
void install_strip_test_binaries(void);

/* Hash a password using crypt(3) with a fresh SHA-512 salt.
 * `out` must be at least 128 bytes.  Returns 0 on success. */
int install_hash_password(const char *password, char *out, int outsz);

/* Write /etc/passwd, /etc/shadow, /etc/group on the currently
 * mounted rootfs.  `username` and `user_hash` may be NULL to skip
 * the optional second user account.  `root_hash` is required. */
int install_write_credentials(const char *root_hash,
                              const char *username,
                              const char *user_hash);

/* One-shot orchestration driver.  Runs every phase in order using
 * the supplied progress struct.  Both the TUI and GUI installer
 * drive this after collecting disk + credentials from the user.
 *
 * Phases, in order:
 *   1. install_write_grub_cfg
 *   2. install_strip_test_binaries
 *   3. install_write_credentials (using the supplied hashes)
 *   4. install_write_gpt
 *   5. install_rescan_gpt
 *   6. install_copy_rootfs (to the rescanned root partition)
 *   7. install_copy_esp
 *   8. sync()
 *
 * Returns 0 on success, -1 on any failure.  On failure, the error
 * callback has been called with a diagnostic and the partially-
 * written disk is left in place (caller may retry or abort). */
int install_run_all(const char *devname, uint64_t disk_blocks,
                    const char *root_hash,
                    const char *username,
                    const char *user_hash,
                    install_progress_t *p);

#endif /* LIBINSTALL_H */
