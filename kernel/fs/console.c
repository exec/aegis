#include "console.h"
#include "vfs.h"
#include "uaccess.h"
#include "arch.h"
#include "../drivers/fb.h"
#include <stdint.h>

#define CONSOLE_BUF 256

/*
 * console_write_fn — write user output to serial + VGA + FB directly.
 *
 * Bypasses printk so that printk_quiet (boot=quiet) does not suppress
 * user-visible output (login banners, shell prompts, command output).
 * Kernel messages go through printk; user output goes through here.
 */
static int
console_write_fn(void *priv, const void *buf, uint64_t len)
{
    (void)priv;
    char kbuf[CONSOLE_BUF];
    uint64_t n = (len > CONSOLE_BUF) ? CONSOLE_BUF : len;
    {
        uint64_t page_off = (uint64_t)(uintptr_t)buf & 0xFFFULL;
        uint64_t to_end   = 0x1000ULL - page_off;
        if (n > to_end)
            n = to_end;
    }
    copy_from_user(kbuf, buf, n);
    /* Null-terminate for string functions */
    kbuf[n] = '\0';
    /* Write directly to serial + VGA + FB, bypassing printk_quiet */
    serial_write_string(kbuf);
    if (vga_available)
        vga_write_string(kbuf);
    if (fb_available)
        fb_write_string(kbuf);
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

static int
console_stat_fn(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode  = S_IFCHR | 0600;
    st->st_ino   = 2;
    st->st_rdev  = makedev(5, 1);  /* /dev/console: major=5 minor=1 */
    st->st_dev   = 1;
    st->st_nlink = 1;
    return 0;
}

static const vfs_ops_t s_console_ops = {
    .read    = console_read_fn,
    .write   = console_write_fn,
    .close   = console_close_fn,
    .readdir = (void *)0,
    .dup     = (void *)0,
    .stat    = console_stat_fn,
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
