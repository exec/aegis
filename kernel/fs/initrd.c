#include "initrd.h"
#include "printk.h"
#include <stdint.h>

/* /etc/motd content — starts with '[' so it survives the make test ANSI filter.
 * The filter keeps only lines starting with '['; content not matching is
 * silently dropped from the serial diff. */
static const char s_motd[] = "[MOTD] Hello from initrd!\n";

typedef struct {
    const char *name;
    const char *data;
    uint32_t    size;
} initrd_entry_t;

static const initrd_entry_t s_files[] = {
    { "/etc/motd", s_motd, sizeof(s_motd) - 1 },
    { (const char *)0, (const char *)0, 0 }  /* sentinel */
};

static const uint32_t s_nfiles = 1;

static int
initrd_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    const initrd_entry_t *e = (const initrd_entry_t *)priv;
    if (off >= e->size) return 0;
    uint64_t avail = e->size - off;
    if (len > avail) len = avail;
    __builtin_memcpy(buf, e->data + off, len);
    return (int)len;
}

static void
initrd_close_fn(void *priv)
{
    (void)priv; /* static data, nothing to free */
}

static const vfs_ops_t initrd_ops = {
    .read  = initrd_read_fn,
    .close = initrd_close_fn,
};

void
initrd_register(void)
{
    printk("[INITRD] OK: %u file registered\n", s_nfiles);
}

int
initrd_open(const char *path, vfs_file_t *out)
{
    uint32_t i;
    for (i = 0; s_files[i].name != (const char *)0; i++) {
        /* manual strcmp — no libc in kernel */
        const char *a = path, *b = s_files[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            out->ops    = &initrd_ops;
            out->priv   = (void *)&s_files[i];
            out->offset = 0;
            return 0;
        }
    }
    return -2; /* ENOENT */
}

const void *
initrd_get_data(const vfs_file_t *f)
{
    if (!f->priv) return (const void *)0;
    return (const void *)((const initrd_entry_t *)f->priv)->data;
}

uint32_t
initrd_get_size(const vfs_file_t *f)
{
    if (!f->priv) return 0;
    return ((const initrd_entry_t *)f->priv)->size;
}
