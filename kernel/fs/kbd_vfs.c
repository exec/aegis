#include "kbd_vfs.h"
#include "vfs.h"
#include "kbd.h"
#include <stdint.h>

static int
kbd_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
	(void)priv; (void)off;
	if (len == 0) return 0;
	char *kbuf = (char *)buf;
	/*
	 * Return exactly 1 byte per call regardless of len.
	 *
	 * POSIX allows read() to return fewer bytes than requested.  Callers
	 * (musl fgets, read loop) must tolerate partial reads.  If we looped
	 * to fill 'len' bytes, musl's fully-buffered stdin would ask for 4096
	 * bytes and block until 4096 keystrokes arrived — making the shell
	 * unusable.  One-byte-at-a-time matches standard Unix tty semantics.
	 */
	kbuf[0] = kbd_read();
	return 1;
}

static int
kbd_vfs_write_fn(void *priv, const void *buf, uint64_t len)
{
	(void)priv; (void)buf; (void)len;
	return -38; /* ENOSYS — stdin is not writable */
}

static void
kbd_vfs_close_fn(void *priv)
{
	(void)priv; /* stateless singleton — nothing to release */
}

static const vfs_ops_t s_kbd_ops = {
	.read    = kbd_vfs_read_fn,
	.write   = kbd_vfs_write_fn,
	.close   = kbd_vfs_close_fn,
	.readdir = (void *)0,
	.dup     = (void *)0,
};

static vfs_file_t s_kbd_file = {
	.ops    = &s_kbd_ops,
	.priv   = (void *)0,
	.offset = 0,
	.size   = 0,
};

vfs_file_t *
kbd_vfs_open(void)
{
	return &s_kbd_file;
}
