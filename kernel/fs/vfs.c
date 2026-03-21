#include "vfs.h"
#include "initrd.h"
#include "printk.h"

void
vfs_init(void)
{
    printk("[VFS] OK: initialized\n");
    initrd_register();
}

int
vfs_open(const char *path, vfs_file_t *out)
{
    return initrd_open(path, out);
}
