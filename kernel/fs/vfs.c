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

static int
streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/*
 * vfs_stat_path — fill *out with stat for the file at path.
 *
 * Handles:
 *   /               → directory (ino=1, mode=S_IFDIR|0555)
 *   /etc            → directory
 *   /bin            → directory
 *   /etc/motd       → initrd file
 *   /bin/sh ...     → initrd file
 *   /dev/console, /dev/tty, /dev/stdin, /dev/stdout, /dev/stderr
 *               → console chardev (mode=S_IFCHR|0600)
 *   /dev/null   → chardev (mode=S_IFCHR|0666, rdev=makedev(1,3))
 *
 * Returns 0 on success, -2 (ENOENT) if not found.
 */
int
vfs_stat_path(const char *path, k_stat_t *out)
{
    if (!path || !out) return -2;

    /* Directory paths */
    if (streq(path, "/") || streq(path, "/etc") || streq(path, "/bin")) {
        __builtin_memset(out, 0, sizeof(*out));
        out->st_dev   = 1;
        out->st_ino   = 1;
        out->st_nlink = 2;
        out->st_mode  = S_IFDIR | 0555;
        return 0;
    }

    /* /dev/ device specials */
    if (streq(path, "/dev/console") || streq(path, "/dev/tty")    ||
        streq(path, "/dev/stdin")   || streq(path, "/dev/stdout") ||
        streq(path, "/dev/stderr")) {
        __builtin_memset(out, 0, sizeof(*out));
        out->st_mode  = S_IFCHR | 0600;
        out->st_ino   = 2;
        out->st_rdev  = makedev(5, 1);
        out->st_dev   = 1;
        out->st_nlink = 1;
        return 0;
    }

    if (streq(path, "/dev/null")) {
        __builtin_memset(out, 0, sizeof(*out));
        out->st_mode  = S_IFCHR | 0666;
        out->st_ino   = 4;
        out->st_rdev  = makedev(1, 3);
        out->st_dev   = 1;
        out->st_nlink = 1;
        return 0;
    }

    /* Initrd file lookup */
    return initrd_stat_entry(path, out);
}
