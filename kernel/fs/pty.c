#include "pty.h"
#include "tty.h"
#include "vfs.h"
#include "uaccess.h"
#include "sched.h"
#include "proc.h"
#include "signal.h"
#include "arch.h"
#include "../core/spinlock.h"
#include <stdint.h>

/* ── Static pool ──────────────────────────────────────────────────── */

static pty_pair_t s_pty_pool[PTY_MAX_PAIRS];
static spinlock_t pty_pool_lock = SPINLOCK_INIT;

/* ── Ring buffer helpers ──────────────────────────────────────────── */

static uint32_t
ring_count(uint32_t head, uint32_t tail)
{
	return (head - tail) & (PTY_BUF_SIZE - 1);
}

static uint32_t
ring_space(uint32_t head, uint32_t tail)
{
	return (PTY_BUF_SIZE - 1) - ring_count(head, tail);
}

static void
ring_push(uint8_t *buf, uint32_t *head, uint8_t ch)
{
	buf[*head] = ch;
	*head = (*head + 1) & (PTY_BUF_SIZE - 1);
}

static uint8_t
ring_pull(uint8_t *buf, uint32_t *tail)
{
	uint8_t ch = buf[*tail];
	*tail = (*tail + 1) & (PTY_BUF_SIZE - 1);
	return ch;
}

/* ── Forward declarations ─────────────────────────────────────────── */

static int  master_read_fn(void *priv, void *buf, uint64_t off, uint64_t len);
static int  master_write_fn(void *priv, const void *buf, uint64_t len);
static void master_dup_fn(void *priv);
static void master_close_fn(void *priv);
static int  master_stat_fn(void *priv, k_stat_t *st);

static int  slave_read_fn(void *priv, void *buf, uint64_t off, uint64_t len);
static int  slave_write_fn(void *priv, const void *buf, uint64_t len);
static void slave_dup_fn(void *priv);
static void slave_close_fn(void *priv);
static int  slave_stat_fn(void *priv, k_stat_t *st);

static const vfs_ops_t s_master_ops = {
	.read    = master_read_fn,
	.write   = master_write_fn,
	.close   = master_close_fn,
	.readdir = (void *)0,
	.dup     = master_dup_fn,
	.stat    = master_stat_fn,
};

static const vfs_ops_t s_slave_ops = {
	.read    = slave_read_fn,
	.write   = slave_write_fn,
	.close   = slave_close_fn,
	.readdir = (void *)0,
	.dup     = slave_dup_fn,
	.stat    = slave_stat_fn,
};

/* ── TTY backend callbacks for the slave side ─────────────────────── */

/*
 * pty_slave_write_out -- called by tty_write/tty_echo to emit processed
 * output.  Pushes bytes into output_buf for the master to read.
 * Returns bytes written or -5 (EIO) if master closed.
 */
static int
pty_slave_write_out(tty_t *tty, const char *buf, uint32_t len)
{
	pty_pair_t *pair = (pty_pair_t *)tty->ctx;
	uint32_t i;

	if (!pair->master_open)
		return -5; /* EIO */

	for (i = 0; i < len; i++) {
		if (ring_space(pair->output_head, pair->output_tail) == 0)
			break; /* output_buf full; partial write */
		ring_push(pair->output_buf, &pair->output_head, (uint8_t)buf[i]);
	}
	/* Wake master reader if blocked */
	if (i > 0 && pair->master_waiting) {
		sched_wake(pair->master_waiting);
		pair->master_waiting = 0;
	}
	return (int)i;
}

/*
 * pty_slave_read_raw -- called by tty_read to get one raw character
 * from the master's input.  Blocks via sched_block until data available,
 * master closes (hangup), or a signal interrupts.
 * Woken by master_write_fn when it pushes data to input_buf.
 *
 * Returns 1 on success (char in *out), 0 if interrupted, -5 on hangup.
 */
static int
pty_slave_read_raw(tty_t *tty, char *out, int *interrupted)
{
	pty_pair_t *pair = (pty_pair_t *)tty->ctx;
	*interrupted = 0;

	for (;;) {
		/* Data available? */
		if (ring_count(pair->input_head, pair->input_tail) > 0) {
			*out = (char)ring_pull(pair->input_buf, &pair->input_tail);
			return 1;
		}
		/* Master closed -- hangup */
		if (!pair->master_open)
			return -5; /* EIO */
		/* Check for pending signals before blocking */
		if (signal_check_pending()) {
			*interrupted = 1;
			return 0;
		}
		/* Block until master writes data or closes */
		pair->slave_waiting = sched_current();
		sched_block();
	}
}

/* pty_slave_poll_raw -- non-blocking single-char poll from the master's
 * input ring buffer.  Returns 1 if a character was available (stored in
 * *out), 0 if the buffer is empty.  Used when VMIN=0. */
static int
pty_slave_poll_raw(tty_t *tty, char *out)
{
	pty_pair_t *pair = (pty_pair_t *)tty->ctx;
	if (ring_count(pair->input_head, pair->input_tail) > 0) {
		*out = (char)ring_pull(pair->input_buf, &pair->input_tail);
		return 1;
	}
	return 0;
}

/* ── Controlling terminal helper ──────────────────────────────────── */

/*
 * try_acquire_ctty -- if the current process is a session leader with
 * no controlling terminal (sid == pid && no tty has this session_id),
 * claim this PTY's tty as the controlling terminal.
 */
static void
try_acquire_ctty(pty_pair_t *pair)
{
	aegis_task_t *t = sched_current();
	aegis_process_t *proc;

	if (!t || !t->is_user)
		return;
	proc = (aegis_process_t *)t;
	/* Only session leaders acquire a ctty */
	if (proc->sid != proc->pid)
		return;
	/* Already have a ctty? */
	if (tty_find_controlling(proc->sid))
		return;
	pair->tty.session_id = proc->sid;
	pair->tty.fg_pgrp = proc->pgid;
}

/* ── Master VFS ops ───────────────────────────────────────────────── */

/*
 * master_read_fn -- read from the output_buf (what the slave wrote).
 * buf is a kernel buffer (kbuf from sys_read). Blocks via sched_block
 * until data available or slave closes.  Woken by pty_slave_write_out.
 * Respects vfs_read_nonblock (O_NONBLOCK): returns -EAGAIN immediately.
 */
static int
master_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	uint32_t n = 0;
	(void)off;

	if (len == 0)
		return 0;

	for (;;) {
		uint32_t avail = ring_count(pair->output_head, pair->output_tail);
		if (avail > 0)
			break;
		/* Slave closed and buffer empty -- EOF */
		if (!pair->slave_open)
			return 0;
		/* O_NONBLOCK: return -EAGAIN instead of blocking */
		if (vfs_read_nonblock)
			return -11; /* EAGAIN */
		/* Check for pending signals */
		if (signal_check_pending())
			return -4; /* EINTR */
		/* Block until slave writes data or closes */
		pair->master_waiting = sched_current();
		sched_block();
	}

	/* Copy out as much as requested or available */
	{
		uint32_t avail = ring_count(pair->output_head, pair->output_tail);
		uint32_t want = (uint32_t)len;
		if (want > avail)
			want = avail;
		for (n = 0; n < want; n++)
			((char *)buf)[n] = (char)ring_pull(pair->output_buf,
			    &pair->output_tail);
	}
	return (int)n;
}

/*
 * master_write_fn -- push bytes into input_buf for the slave to read.
 * buf is a USER pointer -- must use copy_from_user.
 * Page-boundary clamping like console_write_fn.
 */
static int
master_write_fn(void *priv, const void *buf, uint64_t len)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	char kbuf[256];
	uint64_t n;
	uint32_t i;

	if (!pair->slave_open)
		return -5; /* EIO */

	n = (len > 256) ? 256 : len;
	/* Page-boundary clamp */
	{
		uint64_t page_off = (uint64_t)(uintptr_t)buf & 0xFFFULL;
		uint64_t to_end   = 0x1000ULL - page_off;
		if (n > to_end)
			n = to_end;
	}
	copy_from_user(kbuf, buf, n);

	for (i = 0; i < (uint32_t)n; i++) {
		if (ring_space(pair->input_head, pair->input_tail) == 0)
			break; /* input_buf full; partial write */
		ring_push(pair->input_buf, &pair->input_head, (uint8_t)kbuf[i]);
	}
	/* Wake slave reader if blocked */
	if (i > 0 && pair->slave_waiting) {
		sched_wake(pair->slave_waiting);
		pair->slave_waiting = 0;
	}
	return (int)i;
}

static void
master_dup_fn(void *priv)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	irqflags_t fl = spin_lock_irqsave(&pair->lock);
	pair->master_refs++;
	spin_unlock_irqrestore(&pair->lock, fl);
}

static void
master_close_fn(void *priv)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	irqflags_t fl = spin_lock_irqsave(&pair->lock);
	if (pair->master_refs > 1) {
		pair->master_refs--;
		spin_unlock_irqrestore(&pair->lock, fl);
		return;
	}
	pair->master_open = 0;
	pair->master_refs = 0;
	/* Wake slave reader — it will see master_open==0 and return EIO */
	if (pair->slave_waiting) {
		sched_wake(pair->slave_waiting);
		pair->slave_waiting = 0;
	}
	if (!pair->slave_open)
		pair->in_use = 0;
	spin_unlock_irqrestore(&pair->lock, fl);
}

static int
master_stat_fn(void *priv, k_stat_t *st)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	__builtin_memset(st, 0, sizeof(*st));
	st->st_mode  = S_IFCHR | 0600;
	st->st_ino   = 100 + pair->index * 2;
	st->st_rdev  = makedev(5, 2); /* /dev/ptmx: major=5 minor=2 */
	st->st_dev   = 1;
	st->st_nlink = 1;
	return 0;
}

/* ── Slave VFS ops ────────────────────────────────────────────────── */

/*
 * slave_read_fn -- delegates to tty_read which handles cooked/raw mode,
 * echo, signal generation, etc.  buf is a kernel buffer.
 */
static int
slave_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	(void)off;
	if (len == 0)
		return 0;
	return tty_read(&pair->tty, (char *)buf, (uint32_t)len);
}

/*
 * slave_write_fn -- copies from user buffer, then delegates to tty_write
 * which handles OPOST/ONLCR output processing and calls pty_slave_write_out.
 * buf is a USER pointer.
 */
static int
slave_write_fn(void *priv, const void *buf, uint64_t len)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	char kbuf[256];
	uint64_t n;

	if (!pair->master_open)
		return -5; /* EIO */

	n = (len > 256) ? 256 : len;
	/* Page-boundary clamp */
	{
		uint64_t page_off = (uint64_t)(uintptr_t)buf & 0xFFFULL;
		uint64_t to_end   = 0x1000ULL - page_off;
		if (n > to_end)
			n = to_end;
	}
	copy_from_user(kbuf, buf, n);
	return tty_write(&pair->tty, kbuf, (uint32_t)n);
}

static void
slave_dup_fn(void *priv)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	irqflags_t fl = spin_lock_irqsave(&pair->lock);
	pair->slave_refs++;
	spin_unlock_irqrestore(&pair->lock, fl);
}

static void
slave_close_fn(void *priv)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	irqflags_t fl = spin_lock_irqsave(&pair->lock);
	if (pair->slave_refs > 1) {
		pair->slave_refs--;
		spin_unlock_irqrestore(&pair->lock, fl);
		return;
	}
	pair->slave_open = 0;
	pair->slave_refs = 0;
	/* Wake master reader — it will see slave_open==0 and return EOF */
	if (pair->master_waiting) {
		sched_wake(pair->master_waiting);
		pair->master_waiting = 0;
	}
	if (!pair->master_open) {
		pair->in_use = 0;
	}
	spin_unlock_irqrestore(&pair->lock, fl);
}

static int
slave_stat_fn(void *priv, k_stat_t *st)
{
	pty_pair_t *pair = (pty_pair_t *)priv;
	__builtin_memset(st, 0, sizeof(*st));
	st->st_mode  = S_IFCHR | 0620;
	st->st_ino   = 100 + pair->index * 2 + 1;
	st->st_rdev  = makedev(136, pair->index); /* /dev/pts/N: major=136 */
	st->st_dev   = 1;
	st->st_nlink = 1;
	return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

int
ptmx_open(int flags, vfs_file_t *out)
{
	uint32_t i;
	(void)flags;

	irqflags_t fl = spin_lock_irqsave(&pty_pool_lock);
	for (i = 0; i < PTY_MAX_PAIRS; i++) {
		if (!s_pty_pool[i].in_use)
			break;
	}
	if (i == PTY_MAX_PAIRS) {
		spin_unlock_irqrestore(&pty_pool_lock, fl);
		return -12; /* ENOMEM */
	}

	pty_pair_t *pair = &s_pty_pool[i];
	__builtin_memset(pair, 0, sizeof(*pair));
	pair->index = (uint8_t)i;
	pair->in_use = 1;
	pair->master_open = 1;
	pair->master_refs = 1;
	pair->locked = 1; /* cleared by unlockpt (grantpt/unlockpt ioctl) */
	{
		spinlock_t init = SPINLOCK_INIT;
		pair->lock = init;
	}

	tty_init_defaults(&pair->tty);
	pair->tty.write_out = pty_slave_write_out;
	pair->tty.read_raw  = pty_slave_read_raw;
	pair->tty.poll_raw  = pty_slave_poll_raw;
	pair->tty.ctx       = pair;

	try_acquire_ctty(pair);
	spin_unlock_irqrestore(&pty_pool_lock, fl);

	out->ops    = &s_master_ops;
	out->priv   = pair;
	out->offset = 0;
	out->size   = 0;
	out->flags  = 2; /* O_RDWR */
	out->_pad   = 0;
	return 0;
}

int
pts_open(uint32_t index, int flags, vfs_file_t *out)
{
	(void)flags;

	if (index >= PTY_MAX_PAIRS)
		return -2; /* ENOENT */

	irqflags_t fl = spin_lock_irqsave(&pty_pool_lock);
	pty_pair_t *pair = &s_pty_pool[index];
	if (!pair->in_use || !pair->master_open) {
		spin_unlock_irqrestore(&pty_pool_lock, fl);
		return -2; /* ENOENT */
	}
	if (pair->locked) {
		spin_unlock_irqrestore(&pty_pool_lock, fl);
		return -13; /* EACCES */
	}

	pair->slave_open = 1;
	pair->slave_refs = 1;
	try_acquire_ctty(pair);
	spin_unlock_irqrestore(&pty_pool_lock, fl);

	out->ops    = &s_slave_ops;
	out->priv   = pair;
	out->offset = 0;
	out->size   = 0;
	out->flags  = 2; /* O_RDWR */
	out->_pad   = 0;
	return 0;
}

tty_t *
pty_find_by_session(uint32_t session_id)
{
	irqflags_t fl = spin_lock_irqsave(&pty_pool_lock);
	uint32_t i;
	for (i = 0; i < PTY_MAX_PAIRS; i++) {
		if (s_pty_pool[i].in_use &&
		    s_pty_pool[i].tty.session_id == session_id) {
			tty_t *t = &s_pty_pool[i].tty;
			spin_unlock_irqrestore(&pty_pool_lock, fl);
			return t;
		}
	}
	spin_unlock_irqrestore(&pty_pool_lock, fl);
	return (tty_t *)0;
}

int
pty_is_master(const vfs_file_t *f)
{
	return f->ops == &s_master_ops;
}

int
pty_is_slave(const vfs_file_t *f)
{
	return f->ops == &s_slave_ops;
}

tty_t *
pty_get_tty(const vfs_file_t *f)
{
	if (f->ops == &s_slave_ops) {
		pty_pair_t *pair = (pty_pair_t *)f->priv;
		return &pair->tty;
	}
	return (tty_t *)0;
}
