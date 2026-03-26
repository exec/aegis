/* kernel/fs/ramfs.h — in-memory filesystem, multi-instance */
#ifndef AEGIS_RAMFS_H
#define AEGIS_RAMFS_H

#include "vfs.h"
#include <stdint.h>

#define RAMFS_MAX_FILES   32
#define RAMFS_MAX_NAMELEN 64
#define RAMFS_MAX_SIZE    4096  /* one kva page per file */

typedef struct {
    char     name[RAMFS_MAX_NAMELEN];
    uint8_t *data;      /* kva-allocated page; NULL until first write */
    uint32_t size;      /* current byte count */
    uint8_t  in_use;
} ramfs_file_t;

typedef struct {
    ramfs_file_t files[RAMFS_MAX_FILES];
} ramfs_t;

/* ramfs_init — zero the file table. Call from vfs_init() before any open. */
void ramfs_init(ramfs_t *inst);

/* ramfs_open — open or create (if flags & VFS_O_CREAT) a named file.
 * name: short name without the mount prefix (e.g. "vigil.pid", "motd").
 * Returns 0 on success and fills *out; -2 (ENOENT) if not found and
 * VFS_O_CREAT not set; -12 (ENOMEM) if the 32-slot table is full. */
int ramfs_open(ramfs_t *inst, const char *name, int flags, vfs_file_t *out);

/* ramfs_stat — fill *st for a ramfs file given its short name.
 * Returns 0 on success, -2 (ENOENT) if not found. */
int ramfs_stat(ramfs_t *inst, const char *name, k_stat_t *st);

/* ramfs_opendir — open the instance as a directory for getdents64.
 * out->ops = s_ramfs_dir_ops; out->priv = inst.
 * Returns 0 always (directory handle is always valid). */
int ramfs_opendir(ramfs_t *inst, vfs_file_t *out);

/* ramfs_populate — kernel-side write helper. Does NOT call user_ptr_valid.
 * Used by vfs_init() to shadow initrd /etc entries into the etc ramfs.
 * kbuf may be NULL (creates an empty file slot, size=0).
 * Returns 0 on success, -12 (ENOMEM) on allocation failure. */
int ramfs_populate(ramfs_t *inst, const char *name,
                   const uint8_t *kbuf, uint32_t len);

#endif /* AEGIS_RAMFS_H */
