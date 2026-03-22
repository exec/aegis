#include "console.h"
#include "vfs.h"
#include "uaccess.h"
#include "printk.h"
#include <stdint.h>

#define CONSOLE_BUF 256

static int
console_write_fn(void *priv, const void *buf, uint64_t len)
{
    (void)priv;
    char kbuf[CONSOLE_BUF];
    uint64_t n = (len > CONSOLE_BUF) ? CONSOLE_BUF : len;
    /* Cap n so the copy does not cross a page boundary into an unmapped page.
     * buf is a user VA validated by sys_write; the page it occupies is mapped,
     * but the next page (e.g. the guard page above the user stack) may not be.
     * bytes_to_page_end = 0x1000 - (buf & 0xFFF); clamp n to that. */
    {
        uint64_t page_off = (uint64_t)(uintptr_t)buf & 0xFFFULL;
        uint64_t to_end   = 0x1000ULL - page_off;
        if (n > to_end)
            n = to_end;
    }
    /* buf is a user-space pointer, validated by sys_write before dispatch.
     * copy_from_user returns void — no error check possible here. */
    copy_from_user(kbuf, buf, n);
    uint64_t i;
    for (i = 0; i < n; i++)
        printk("%c", kbuf[i]);
    /* Return actual bytes written, not requested len. */
    return (int)n;
}

static int
console_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)buf; (void)off; (void)len;
    return -38; /* ENOSYS — stdin not implemented */
}

static void
console_close_fn(void *priv)
{
    (void)priv; /* stateless singleton — nothing to free */
}

static const vfs_ops_t s_console_ops = {
    .read    = console_read_fn,
    .write   = console_write_fn,
    .close   = console_close_fn,
    .readdir = (void *)0,
};

static vfs_file_t s_console_file = {
    .ops    = &s_console_ops,
    .priv   = (void *)0,
    .offset = 0,
    .size   = 0,
};

void
console_init(void)
{
    /* Stateless singleton — nothing to initialize. */
}

vfs_file_t *
console_open(void)
{
    return &s_console_file;
}
