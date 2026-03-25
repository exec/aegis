#include "kbd_vfs.h"
#include "vfs.h"
#include "kbd.h"
#include "proc.h"
#include "sched.h"
#include "printk.h"
#include "uaccess.h"
#include "syscall_util.h"
#include <stdint.h>

/* k_termios_t — kernel copy matching musl x86_64 struct termios (60 bytes).
 * Layout: c_iflag(4)+c_oflag(4)+c_cflag(4)+c_lflag(4)+c_line(1)+c_cc[32](32)
 *         +3_pad+c_ispeed(4)+c_ospeed(4) = 60 bytes.
 * Must match musl's struct termios so TCGETS/TCSETS exchange works directly. */
#define K_NCCS   32
#define K_ICANON 0x02U   /* c_lflag: canonical (line-buffered) mode */
#define K_ECHO   0x08U   /* c_lflag: echo input characters */
#define K_ISIG   0x01U   /* c_lflag: enable INTR/QUIT signal generation */
#define K_ICRNL  0x100U  /* c_iflag: translate CR to NL on input */
#define K_VMIN   6       /* c_cc index: minimum bytes for raw read */
#define K_VTIME  5       /* c_cc index: timeout for raw read (tenths of sec) */

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[K_NCCS];
    /* 3 bytes natural padding between c_cc[32] (offset 49) and c_ispeed (offset 52) */
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} k_termios_t;

_Static_assert(sizeof(k_termios_t) == 60,
    "k_termios_t must be 60 bytes to match musl struct termios");

static k_termios_t s_termios;   /* kernel terminal attributes */
static int         s_raw = 0;   /* cached: !(c_lflag & K_ICANON) */

/* Simple line discipline — echoes characters as typed, handles backspace.
 * Buffers one line at a time; returns characters one-by-one to the caller.
 * TTY termios layer: s_raw=0 → cooked (canonical), s_raw=1 → raw. */
#define KBD_LINE_MAX 512
static char s_linebuf[KBD_LINE_MAX];
static int  s_linebuf_len = 0;  /* total bytes ready in buffer (incl. \n) */
static int  s_linebuf_pos = 0;  /* next byte to return to caller */

static void
kbd_vfs_termios_init(void)
{
    __builtin_memset(&s_termios, 0, sizeof(s_termios));
    s_termios.c_iflag        = K_ICRNL;
    s_termios.c_lflag        = K_ICANON | K_ECHO | K_ISIG;
    s_termios.c_cc[K_VMIN]  = 1;
    s_termios.c_cc[K_VTIME] = 0;
    s_raw = 0;
}

static int
kbd_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
	(void)priv; (void)off;
	if (len == 0) return 0;
	char *kbuf = (char *)buf;

	/* RAW mode: one character immediately, no echo, no line buffer */
	if (s_raw) {
		int interrupted;
		char c = kbd_read_interruptible(&interrupted);
		if (interrupted) return -4; /* EINTR */
		kbuf[0] = c;
		return 1;
	}

	/* COOKED mode: return buffered bytes from a previously-read line */
	if (s_linebuf_pos < s_linebuf_len) {
		kbuf[0] = s_linebuf[s_linebuf_pos++];
		if (s_linebuf_pos >= s_linebuf_len) {
			s_linebuf_len = 0;
			s_linebuf_pos = 0;
		}
		return 1;
	}

	/* COOKED mode: read and echo characters until Enter */
	for (;;) {
		int  interrupted;
		char c = kbd_read_interruptible(&interrupted);
		if (interrupted) {
			s_linebuf_len = 0;
			s_linebuf_pos = 0;
			return -4; /* EINTR */
		}
		if (c == '\r' || c == '\n') {
			if (s_linebuf_len < KBD_LINE_MAX - 1)
				s_linebuf[s_linebuf_len++] = '\n';
			printk("\n");
			break;
		} else if (c == '\x7f' || c == '\x08') {
			if (s_linebuf_len > 0) {
				s_linebuf_len--;
				printk("\b \b");
			}
		} else if ((uint8_t)c >= 0x20 && s_linebuf_len < KBD_LINE_MAX - 1) {
			s_linebuf[s_linebuf_len++] = c;
			printk("%c", c);
		}
	}

	kbuf[0] = s_linebuf[s_linebuf_pos++];
	if (s_linebuf_pos >= s_linebuf_len) {
		s_linebuf_len = 0;
		s_linebuf_pos = 0;
	}
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

static int
kbd_stat_fn(void *priv, k_stat_t *st)
{
	(void)priv;
	__builtin_memset(st, 0, sizeof(*st));
	st->st_mode  = S_IFCHR | 0400;
	st->st_ino   = 3;
	st->st_rdev  = makedev(4, 0);  /* /dev/tty: major=4 minor=0 */
	st->st_dev   = 1;
	st->st_nlink = 1;
	return 0;
}

static const vfs_ops_t s_kbd_ops = {
	.read    = kbd_vfs_read_fn,
	.write   = kbd_vfs_write_fn,
	.close   = kbd_vfs_close_fn,
	.readdir = (void *)0,
	.dup     = (void *)0,
	.stat    = kbd_stat_fn,
};

static vfs_file_t s_kbd_file = {
	.ops    = &s_kbd_ops,
	.priv   = (void *)0,
	.offset = 0,
	.size   = 0,
};

/* kbd_vfs_tcgets — copy kernel termios to user pointer.
 * Validates pointer before any copy. Returns 0 or negative errno. */
int
kbd_vfs_tcgets(void *dst_user)
{
    if (!user_ptr_valid((uint64_t)(uintptr_t)dst_user, sizeof(k_termios_t)))
        return -14; /* EFAULT */
    copy_to_user(dst_user, &s_termios, sizeof(k_termios_t));
    return 0;
}

/* kbd_vfs_tcsets — copy termios from user pointer to kernel.
 * Updates s_raw cached flag atomically with the store. */
int
kbd_vfs_tcsets(const void *src_user)
{
    if (!user_ptr_valid((uint64_t)(uintptr_t)src_user, sizeof(k_termios_t)))
        return -14; /* EFAULT */
    k_termios_t tmp;
    copy_from_user(&tmp, src_user, sizeof(k_termios_t));
    s_termios = tmp;
    /* s_raw is always derived from c_lflag — never an independent state */
    s_raw = (s_termios.c_lflag & K_ICANON) ? 0 : 1;
    return 0;
}

/* kbd_vfs_is_tty — returns 1 if vfs_file uses the kbd_vfs ops (is a tty). */
int
kbd_vfs_is_tty(const vfs_file_t *f)
{
    return f->ops == &s_kbd_ops;
}

vfs_file_t *
kbd_vfs_open(void)
{
    static int s_inited = 0;
    if (!s_inited) {
        kbd_vfs_termios_init();
        s_inited = 1;
    }
    return &s_kbd_file;
}
