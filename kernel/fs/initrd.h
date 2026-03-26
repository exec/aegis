#ifndef INITRD_H
#define INITRD_H

#include "vfs.h"

/* initrd_register — print [INITRD] OK line.
 * Called from vfs_init. */
void initrd_register(void);

/* initrd_open — populate *out if path matches a known file.
 * Returns 0 on success, -2 (ENOENT) if not found. */
int initrd_open(const char *path, vfs_file_t *out);

/* initrd_get_data — return pointer to raw file data for a file previously
 * opened with initrd_open.  f->priv must be an initrd_entry_t pointer.
 * Returns NULL if f->priv is NULL. */
const void *initrd_get_data(const vfs_file_t *f);

/* initrd_get_size — return byte size of a file previously opened with
 * initrd_open.  Returns 0 if f->priv is NULL. */
uint32_t initrd_get_size(const vfs_file_t *f);

/* initrd_stat_entry — fill *out with stat for the initrd file at path.
 * Returns 0 if found, -2 (ENOENT) if not found.
 * Used by vfs_stat_path to avoid re-opening the file. */
int initrd_stat_entry(const char *path, k_stat_t *out);

/* initrd_iter_etc — iterate all initrd entries whose path starts with "/etc/".
 * For each such entry, cb is called with:
 *   path  — the short name without the "/etc/" prefix (e.g. "motd", "passwd",
 *            "vigil/services/getty/run")
 *   data  — pointer to file content (kernel static data)
 *   len   — file length in bytes
 *   ud    — opaque user data passed through unchanged
 * Used by vfs_init() to populate the etc ramfs from initrd at boot. */
typedef void (*initrd_etc_cb_t)(const char *path, const uint8_t *data,
                                uint32_t len, void *ud);
void initrd_iter_etc(initrd_etc_cb_t cb, void *ud);

#endif /* INITRD_H */
