/* kernel/fs/ramfs.c — in-memory filesystem for /run */
#include "ramfs.h"
#include "kva.h"
#include "printk.h"
#include "uaccess.h"
#include "syscall_util.h"
#include <stdint.h>

#define RAMFS_MAX_FILES   16
#define RAMFS_MAX_NAMELEN 64
#define RAMFS_MAX_SIZE    4096   /* one kva page per file */

typedef struct {
    char     name[RAMFS_MAX_NAMELEN];
    uint8_t *data;      /* kva-allocated page; NULL until first write */
    uint32_t size;      /* current byte count */
    uint8_t  in_use;
} ramfs_file_t;

static ramfs_file_t s_ramfs[RAMFS_MAX_FILES];

/* ── helpers ──────────────────────────────────────────────────────────── */

static int
rfs_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void
rfs_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static ramfs_file_t *
rfs_find(const char *name)
{
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++)
        if (s_ramfs[i].in_use && rfs_streq(s_ramfs[i].name, name))
            return &s_ramfs[i];
    return (ramfs_file_t *)0;
}

static ramfs_file_t *
rfs_alloc(const char *name)
{
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!s_ramfs[i].in_use) {
            rfs_strcpy(s_ramfs[i].name, name, RAMFS_MAX_NAMELEN);
            s_ramfs[i].data   = (uint8_t *)0;
            s_ramfs[i].size   = 0;
            s_ramfs[i].in_use = 1;
            return &s_ramfs[i];
        }
    }
    return (ramfs_file_t *)0;
}

/* ── vfs_ops_t callbacks ─────────────────────────────────────────────── */

static int
ramfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    ramfs_file_t *f = (ramfs_file_t *)priv;
    if (!f->data || off >= f->size) return 0;  /* EOF */
    uint64_t avail = f->size - off;
    if (len > avail) len = avail;
    uint32_t n = (uint32_t)len;  /* safe: len <= f->size <= RAMFS_MAX_SIZE=4096 */
    uint8_t *dst = (uint8_t *)buf;
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = f->data[off + i];
    return (int)n;
}

static int
ramfs_write_fn(void *priv, const void *buf, uint64_t len)
{
    ramfs_file_t *f = (ramfs_file_t *)priv;
    /* Validate user pointer before copy_from_user.
     * sys_write already calls user_ptr_valid at the syscall layer, but we
     * guard here too because ramfs_write_fn may be called from other paths
     * (e.g. future in-kernel writers) that bypass that check. */
    if (!user_ptr_valid((uintptr_t)buf, len)) return -14;  /* EFAULT */
    if (len > RAMFS_MAX_SIZE) len = RAMFS_MAX_SIZE;
    /* Allocate backing page on first write */
    if (!f->data) {
        f->data = (uint8_t *)kva_alloc_pages(1);
        if (!f->data) return -12;  /* ENOMEM */
    }
    /* O_TRUNC semantics: always write from offset 0; append not supported.
     * buf is a user-space pointer (SMAP is active).  Copy via copy_from_user
     * in page-bounded chunks to avoid crossing an unmapped page boundary. */
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        /* Cap to current page boundary */
        {
            uint64_t page_off = (uint64_t)(uintptr_t)((const uint8_t *)buf + done) & 0xFFFULL;
            uint64_t to_end   = 0x1000ULL - page_off;
            if (chunk > to_end)
                chunk = to_end;
        }
        copy_from_user(f->data + done, (const uint8_t *)buf + done, chunk);
        done += chunk;
    }
    f->size = (uint32_t)len;
    return (int)len;
}

static void
ramfs_close_fn(void *priv)
{
    (void)priv;  /* data persists; file lives until kernel shuts down */
}

static int
ramfs_readdir_fn(void *priv, uint64_t index, char *name_out, uint8_t *type_out)
{
    (void)priv;
    uint64_t found = 0;
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!s_ramfs[i].in_use) continue;
        if (found == index) {
            rfs_strcpy(name_out, s_ramfs[i].name, RAMFS_MAX_NAMELEN);
            *type_out = 8;  /* DT_REG */
            return 0;
        }
        found++;
    }
    return -1;  /* past last entry */
}

static int
ramfs_stat_fn(void *priv, k_stat_t *st)
{
    ramfs_file_t *f = (ramfs_file_t *)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 3;          /* ramfs device id */
    st->st_ino   = 1;
    st->st_nlink = 1;
    st->st_mode  = S_IFREG | 0644;
    st->st_size  = (int64_t)f->size;
    return 0;
}

static const vfs_ops_t s_ramfs_ops = {
    .read    = ramfs_read_fn,
    .write   = ramfs_write_fn,
    .close   = ramfs_close_fn,
    .readdir = ramfs_readdir_fn,
    .dup     = (void *)0,   /* stateless priv pointer — no refcount needed */
    .stat    = ramfs_stat_fn,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

void
ramfs_init(void)
{
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++) {
        s_ramfs[i].name[0] = '\0';
        s_ramfs[i].data    = (uint8_t *)0;
        s_ramfs[i].size    = 0;
        s_ramfs[i].in_use  = 0;
    }
}

int
ramfs_open(const char *name, int flags, vfs_file_t *out)
{
    ramfs_file_t *f = rfs_find(name);
    if (!f) {
        if (!(flags & (int)VFS_O_CREAT)) return -2;  /* ENOENT */
        f = rfs_alloc(name);
        if (!f) return -12;  /* ENOMEM */
    }
    out->ops    = &s_ramfs_ops;
    out->priv   = (void *)f;
    out->offset = 0;
    out->size   = (uint64_t)f->size;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

int
ramfs_stat_path(const char *name, k_stat_t *st)
{
    ramfs_file_t *f = rfs_find(name);
    if (!f) return -2;  /* ENOENT */
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 3;
    st->st_ino   = 1;
    st->st_nlink = 1;
    st->st_mode  = S_IFREG | 0644;
    st->st_size  = (int64_t)f->size;
    return 0;
}
