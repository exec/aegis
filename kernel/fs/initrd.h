#ifndef INITRD_H
#define INITRD_H

#include "vfs.h"

/* initrd_register — print [INITRD] OK line.
 * Called from vfs_init. */
void initrd_register(void);

/* initrd_open — populate *out if path matches a known file.
 * Returns 0 on success, -2 (ENOENT) if not found. */
int initrd_open(const char *path, vfs_file_t *out);

#endif /* INITRD_H */
