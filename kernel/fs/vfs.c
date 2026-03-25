#include "vfs.h"
#include "initrd.h"
#include "ext2.h"
#include "ramfs.h"
#include "printk.h"
#include "uaccess.h"
#include <stdint.h>

void
vfs_init(void)
{
    ramfs_init();
    printk("[VFS] OK: initialized\n");
    initrd_register();
}

/* ── ext2 fd private state ────────────────────────────────────────────── */

/*
 * ext2_fd_priv_t — per-open-file state for ext2 fds.
 *
 * We need a struct rather than a raw inode pointer because:
 *   - ops->write(priv, buf, len) does not receive the current offset.
 *   - We must track write_offset ourselves so sequential writes work.
 *   - sys_read passes f->offset (maintained by sys_read), so reads are
 *     offset-driven from the fd; we just forward to ext2_read().
 *   - sys_write does NOT maintain f->offset, so write position is private.
 *
 * Allocated from s_ext2_pool[] (32 slots); freed on close.
 */
typedef struct {
    uint32_t ino;           /* ext2 inode number */
    uint32_t write_offset;  /* current sequential write position */
    uint32_t in_use;        /* 1 if slot is occupied */
    uint32_t ref_count;     /* number of open fds sharing this slot */
} ext2_fd_priv_t;

#define EXT2_FD_POOL 32
static ext2_fd_priv_t s_ext2_pool[EXT2_FD_POOL];

static ext2_fd_priv_t *
ext2_pool_alloc(uint32_t ino)
{
    uint32_t i;
    for (i = 0; i < EXT2_FD_POOL; i++) {
        if (!s_ext2_pool[i].in_use) {
            s_ext2_pool[i].ino          = ino;
            s_ext2_pool[i].write_offset = 0;
            s_ext2_pool[i].in_use       = 1;
            s_ext2_pool[i].ref_count    = 1;
            return &s_ext2_pool[i];
        }
    }
    return (ext2_fd_priv_t *)0;
}

static void
ext2_pool_free(ext2_fd_priv_t *p)
{
    if (!p) return;
    if (p->ref_count > 0)
        p->ref_count--;
    if (p->ref_count == 0)
        p->in_use = 0;
}

/* ── ext2 vfs_ops_t implementations ──────────────────────────────────── */

static int
ext2_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    ext2_fd_priv_t *p = (ext2_fd_priv_t *)priv;
    /* sys_read passes f->offset as off; forward directly. */
    return ext2_read(p->ino, buf, (uint32_t)off, (uint32_t)len);
}

/* EXT2_WRITE_CHUNK — maximum bytes copied per write pass.
 * Matches NVMe single-transfer limit (one 4 KB block).  buf is a user-space
 * pointer passed from sys_write; copy_from_user transfers it to a kernel
 * bounce buffer before calling ext2_write, avoiding a SMAP violation. */
#define EXT2_WRITE_CHUNK 4096

static int
ext2_vfs_write_fn(void *priv, const void *buf, uint64_t len)
{
    ext2_fd_priv_t *p = (ext2_fd_priv_t *)priv;
    /* sys_write does not pass a file offset, so we maintain write_offset
     * in the priv struct for sequential writes.  This is correct for:
     *   - shell redirection (O_WRONLY|O_CREAT|O_TRUNC): write from 0.
     *   - append writes: each call advances write_offset by bytes written.
     * Concurrent writes to the same fd are not supported (single-process).
     *
     * buf is a user-space pointer (validated by sys_write before dispatch).
     * ext2_write accesses it in kernel mode, which SMAP forbids.  Copy into
     * a kernel bounce buffer first.  Loop in EXT2_WRITE_CHUNK slices to keep
     * the stack frame bounded. */
    static uint8_t s_kbuf[EXT2_WRITE_CHUNK];   /* kernel bounce buffer */
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        if (chunk > EXT2_WRITE_CHUNK)
            chunk = EXT2_WRITE_CHUNK;
        /* Cap chunk to the current page boundary so we never cross an
         * unmapped page (same guard as console_write_fn). */
        {
            uint64_t page_off = (uint64_t)(uintptr_t)((const uint8_t *)buf + done) & 0xFFFULL;
            uint64_t to_end   = 0x1000ULL - page_off;
            if (chunk > to_end)
                chunk = to_end;
        }
        copy_from_user(s_kbuf, (const uint8_t *)buf + done, (uint32_t)chunk);
        int n = ext2_write(p->ino, s_kbuf, p->write_offset, (uint32_t)chunk);
        if (n <= 0)
            return (done > 0) ? (int)done : n;
        p->write_offset += (uint32_t)n;
        done += (uint64_t)n;
    }
    return (int)done;
}

static void
ext2_vfs_close_fn(void *priv)
{
    ext2_pool_free((ext2_fd_priv_t *)priv);
}

static void
ext2_vfs_dup_fn(void *priv)
{
    ext2_fd_priv_t *p = (ext2_fd_priv_t *)priv;
    if (p)
        p->ref_count++;
}

static int
ext2_vfs_readdir_fn(void *priv, uint64_t index,
                    char *name_out, uint8_t *type_out)
{
    ext2_fd_priv_t *p = (ext2_fd_priv_t *)priv;
    return ext2_readdir(p->ino, index, name_out, type_out);
}

static int
ext2_vfs_stat_fn(void *priv, k_stat_t *st)
{
    ext2_fd_priv_t *p = (ext2_fd_priv_t *)priv;
    int sz = ext2_file_size(p->ino);
    if (sz < 0) return -1;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev     = 2;           /* device 2 = nvme0 */
    st->st_ino     = (uint64_t)p->ino;
    st->st_nlink   = 1;
    if (ext2_is_dir(p->ino))
        st->st_mode = S_IFDIR | 0755;
    else
        st->st_mode = S_IFREG | 0644;
    st->st_size    = (int64_t)sz;
    st->st_blksize = 4096;
    st->st_blocks  = (int64_t)(((uint64_t)sz + 511) / 512 * 8);
    return 0;
}

static const vfs_ops_t s_ext2_ops = {
    .read    = ext2_vfs_read_fn,
    .write   = ext2_vfs_write_fn,
    .close   = ext2_vfs_close_fn,
    .readdir = ext2_vfs_readdir_fn,
    .dup     = ext2_vfs_dup_fn,
    .stat    = ext2_vfs_stat_fn,
};

/* ── vfs_open ─────────────────────────────────────────────────────────── */

/*
 * vfs_open — resolve path to a vfs_file_t across all registered backends.
 *
 * Priority order:
 *   1. initrd (static in-kernel filesystem: /bin, /etc/motd, /dev)
 *   2. ext2 on nvme0 (if mounted)
 *
 * flags: open flags forwarded from sys_open.  VFS_O_CREAT causes vfs_open
 *        to call ext2_create() if the file is not found on ext2.
 *
 * Returns 0 on success, -2 (ENOENT) if not found, -12 (ENOMEM) if the
 * ext2 fd pool is exhausted.
 */
int
vfs_open(const char *path, int flags, vfs_file_t *out)
{
    /* Try initrd first — handles /bin, /etc/motd, /dev/console, /dev/kbd */
    if (initrd_open(path, out) == 0)
        return 0;

    /* Route /run/ paths to ramfs */
    {
        const char *p = path;
        /* /run/ file routing: p[4] is '\0' for bare "/run" so that case
         * falls through to the streq directory check above */
        if (p[0]=='/' && p[1]=='r' && p[2]=='u' && p[3]=='n' && p[4]=='/')
            return ramfs_open(path + 5, flags, out);
    }

    /* Fall through to ext2.  ext2_open() returns -1 if not mounted or
     * the file does not exist. */
    uint32_t ino = 0;
    if (ext2_open(path, &ino) < 0) {
        /* If O_CREAT is set, create the file and try again. */
        if ((flags & (int)VFS_O_CREAT) && ext2_create(path, 0644) == 0) {
            if (ext2_open(path, &ino) < 0)
                return -2;  /* ENOENT — create succeeded but open failed */
        } else {
            return -2;  /* ENOENT */
        }
    }

    ext2_fd_priv_t *p = ext2_pool_alloc(ino);
    if (!p)
        return -12;  /* ENOMEM — fd pool exhausted */

    int sz = ext2_file_size(ino);
    if (sz < 0) sz = 0;

    out->ops    = &s_ext2_ops;
    out->priv   = (void *)p;
    out->offset = 0;
    out->size   = (uint64_t)sz;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

/* ── helpers ──────────────────────────────────────────────────────────── */

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
 *   everything else → ext2 lookup (if mounted)
 *
 * Returns 0 on success, -2 (ENOENT) if not found.
 */
int
vfs_stat_path(const char *path, k_stat_t *out)
{
    if (!path || !out) return -2;

    /* Directory paths */
    if (streq(path, "/")    || streq(path, "/etc")  || streq(path, "/bin") ||
        streq(path, "/dev") || streq(path, "/root") || streq(path, "/run")) {
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

    /* ramfs lookup for /run/ paths — pass short name without /run/ prefix */
    {
        const char *p = path;
        /* /run/ file routing: p[4] is '\0' for bare "/run" so that case
         * falls through to the streq directory check above */
        if (p[0]=='/' && p[1]=='r' && p[2]=='u' && p[3]=='n' && p[4]=='/')
            return ramfs_stat_path(path + 5, out);
    }

    /* Initrd file lookup */
    if (initrd_stat_entry(path, out) == 0)
        return 0;

    /* ext2 fallback */
    {
        uint32_t ino = 0;
        if (ext2_open(path, &ino) == 0) {
            int sz = ext2_file_size(ino);
            if (sz < 0) sz = 0;
            __builtin_memset(out, 0, sizeof(*out));
            out->st_dev     = 2;
            out->st_ino     = (uint64_t)ino;
            out->st_nlink   = 1;
            if (ext2_is_dir(ino))
                out->st_mode = S_IFDIR | 0755;
            else
                out->st_mode = S_IFREG | 0644;
            out->st_size    = (int64_t)sz;
            out->st_blksize = 4096;
            out->st_blocks  = (int64_t)(((uint64_t)sz + 511) / 512 * 8);
            return 0;
        }
    }

    return -2;
}
