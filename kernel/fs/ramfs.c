/* kernel/fs/ramfs.c — in-memory filesystem, multi-instance */
#include "ramfs.h"
#include "kva.h"
#include "printk.h"
#include "uaccess.h"
#include "syscall_util.h"
#include <stdint.h>

/* RAMFS_MAX_FILES, RAMFS_MAX_NAMELEN, RAMFS_MAX_SIZE defined in ramfs.h */

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
rfs_find(ramfs_t *inst, const char *name)
{
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++)
        if (inst->files[i].in_use && rfs_streq(inst->files[i].name, name))
            return &inst->files[i];
    return (ramfs_file_t *)0;
}

static ramfs_file_t *
rfs_alloc(ramfs_t *inst, const char *name)
{
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!inst->files[i].in_use) {
            rfs_strcpy(inst->files[i].name, name, RAMFS_MAX_NAMELEN);
            inst->files[i].data   = (uint8_t *)0;
            inst->files[i].size   = 0;
            inst->files[i].in_use = 1;
            return &inst->files[i];
        }
    }
    return (ramfs_file_t *)0;
}

/* ── vfs_ops_t callbacks (file handles — priv is ramfs_file_t *) ─────── */

static int
ramfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    ramfs_file_t *f = (ramfs_file_t *)priv;
    if (!f->data || off >= f->size) return 0;
    uint64_t avail = f->size - off;
    if (len > avail) len = avail;
    uint32_t n = (uint32_t)len;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = f->data[(uint32_t)off + i];
    return (int)n;
}

static int
ramfs_write_fn(void *priv, const void *buf, uint64_t len)
{
    ramfs_file_t *f = (ramfs_file_t *)priv;
    /* Buffer may be a user pointer (sys_write) or a kernel staging buffer
     * (sys_writev).  The syscall layer validates user pointers before calling
     * us, so we accept both here.  copy_from_user (stac/memcpy/clac) works
     * correctly on kernel addresses — STAC enables user access but kernel
     * access is always permitted. */
    if (len > RAMFS_MAX_SIZE) len = RAMFS_MAX_SIZE;
    if (!f->data) {
        f->data = (uint8_t *)kva_alloc_pages(1);
        if (!f->data) return -12;
    }
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        /* For user pointers, cap copy to page boundary to avoid crossing
         * an unmapped page.  For kernel pointers this is harmless. */
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
    (void)priv;
}

static int
ramfs_stat_fn(void *priv, k_stat_t *st)
{
    ramfs_file_t *f = (ramfs_file_t *)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 3;
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
    .readdir = 0,   /* files are not directories */
    .dup     = 0,
    .stat    = ramfs_stat_fn,
};

/* ── vfs_ops_t callbacks (directory handles — priv is ramfs_t *) ─────── */

static int
ramfs_dir_readdir_fn(void *priv, uint64_t index, char *name_out, uint8_t *type_out)
{
    ramfs_t *inst = (ramfs_t *)priv;
    uint64_t found = 0;
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!inst->files[i].in_use) continue;
        if (found == index) {
            rfs_strcpy(name_out, inst->files[i].name, RAMFS_MAX_NAMELEN);
            *type_out = 8;  /* DT_REG */
            return 0;
        }
        found++;
    }
    return -1;
}

static const vfs_ops_t s_ramfs_dir_ops = {
    .read    = 0,   /* EISDIR */
    .write   = 0,   /* EISDIR */
    .close   = ramfs_close_fn,
    .readdir = ramfs_dir_readdir_fn,
    .dup     = 0,
    .stat    = 0,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

void
ramfs_init(ramfs_t *inst)
{
    uint32_t i;
    for (i = 0; i < RAMFS_MAX_FILES; i++) {
        inst->files[i].name[0] = '\0';
        inst->files[i].data    = (uint8_t *)0;
        inst->files[i].size    = 0;
        inst->files[i].in_use  = 0;
    }
}

int
ramfs_open(ramfs_t *inst, const char *name, int flags, vfs_file_t *out)
{
    ramfs_file_t *f = rfs_find(inst, name);
    if (!f) {
        if (!(flags & (int)VFS_O_CREAT)) return -2;
        f = rfs_alloc(inst, name);
        if (!f) return -12;
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
ramfs_stat(ramfs_t *inst, const char *name, k_stat_t *st)
{
    ramfs_file_t *f = rfs_find(inst, name);
    if (!f) return -2;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 3;
    st->st_ino   = 1;
    st->st_nlink = 1;
    st->st_mode  = S_IFREG | 0644;
    st->st_size  = (int64_t)f->size;
    return 0;
}

int
ramfs_opendir(ramfs_t *inst, vfs_file_t *out)
{
    out->ops    = &s_ramfs_dir_ops;
    out->priv   = (void *)inst;
    out->offset = 0;
    out->size   = 0;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

int
ramfs_populate(ramfs_t *inst, const char *name,
               const uint8_t *kbuf, uint32_t len)
{
    ramfs_file_t *f = rfs_find(inst, name);
    if (!f) {
        f = rfs_alloc(inst, name);
        if (!f) return -12;
    }
    if (len == 0) { f->size = 0; return 0; }
    if (!f->data) {
        f->data = (uint8_t *)kva_alloc_pages(1);
        if (!f->data) return -12;
    }
    if (len > RAMFS_MAX_SIZE) len = RAMFS_MAX_SIZE;
    __builtin_memcpy(f->data, kbuf, len);
    f->size = len;
    return 0;
}
