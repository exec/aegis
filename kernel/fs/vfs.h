#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define PROC_MAX_FDS 8

/* File operations vtable. Each open file carries a pointer to its driver's ops. */
typedef struct {
    /* read — copy up to len bytes starting at off into buf (kernel buffer).
     * Returns bytes copied (0 = EOF, negative = error). */
    int (*read)(void *priv, void *buf, uint64_t off, uint64_t len);
    /* write — copy len bytes from user-space buf to device.
     * Returns bytes written, or negative errno. NULL = not writable. */
    int (*write)(void *priv, const void *buf, uint64_t len);
    /* close — release any driver-side resources for this file. */
    void (*close)(void *priv);
} vfs_ops_t;

/* Open file descriptor. Embedded inline in aegis_process_t.fds[].
 * ops == NULL means the slot is free. */
typedef struct {
    const vfs_ops_t *ops;    /* NULL = free slot */
    void            *priv;   /* driver-private data */
    uint64_t         offset; /* current read position */
} vfs_file_t;

/* vfs_init — print [VFS] OK line and register built-in drivers.
 * Called from kernel_main before sched_init. */
void vfs_init(void);

/* vfs_open — find a file by path across all registered drivers.
 * Populates *out on success; returns 0 on success, -2 (ENOENT) if not found.
 * Called by sys_open to resolve path to a vfs_file_t. */
int vfs_open(const char *path, vfs_file_t *out);

#endif /* VFS_H */
