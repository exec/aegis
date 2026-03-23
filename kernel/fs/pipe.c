#include "pipe.h"
#include "vfs.h"
#include "sched.h"
#include "kva.h"
#include "uaccess.h"
#include <stdint.h>
#include <stddef.h>

_Static_assert(sizeof(pipe_t) == 4096,
    "pipe_t must be exactly 4096 bytes (one kva page); adjust PIPE_BUF_SIZE");

/* Forward declarations */
static int  pipe_read_fn(void *priv, void *buf, uint64_t off, uint64_t len);
static int  pipe_write_fn(void *priv, const void *buf, uint64_t len);
static void pipe_read_close_fn(void *priv);
static void pipe_write_close_fn(void *priv);
static void pipe_dup_read_fn(void *priv);
static void pipe_dup_write_fn(void *priv);
static int  pipe_stat_fn(void *priv, k_stat_t *st);

const vfs_ops_t g_pipe_read_ops = {
    .read    = pipe_read_fn,
    .write   = (void *)0,
    .close   = pipe_read_close_fn,
    .readdir = (void *)0,
    .dup     = pipe_dup_read_fn,
    .stat    = pipe_stat_fn,
};

const vfs_ops_t g_pipe_write_ops = {
    .read    = (void *)0,
    .write   = pipe_write_fn,
    .close   = pipe_write_close_fn,
    .readdir = (void *)0,
    .dup     = pipe_dup_write_fn,
    .stat    = pipe_stat_fn,
};

/*
 * pipe_read_fn — read up to len bytes from the pipe into kernel buffer buf.
 *
 * Blocking semantics (retry-as-loop):
 *   - If empty and write end closed: return 0 (EOF).
 *   - If empty and write end open: block until data arrives or write end closes.
 *   - After sched_block() returns, execution resumes at `continue` in the loop
 *     and the conditions are re-evaluated. Same pattern as sys_waitpid.
 *
 * buf is a kernel buffer (kbuf inside sys_read), NOT a user pointer. Plain memcpy.
 * offset is ignored — pipes have no seek position.
 */
static int
pipe_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    pipe_t *p = (pipe_t *)priv;
    (void)off;

    for (;;) {
        if (p->count == 0 && p->write_refs == 0)
            return 0;   /* EOF: all writers gone */
        if (p->count == 0) {
            /* Block until a writer deposits data or closes the write end. */
            p->reader_waiting = sched_current();
            sched_block();
            /* Resumes here after sched_wake(); re-check conditions. */
            continue;
        }
        break;
    }

    /* Data is available. Copy min(len, count) bytes out of the ring. */
    uint32_t n = (uint32_t)len;
    if (n > p->count) n = p->count;

    /* Ring buffer read with wrap-around */
    uint32_t first = PIPE_BUF_SIZE - p->read_pos;
    if (first > n) first = n;
    __builtin_memcpy(buf, &p->buf[p->read_pos], first);
    if (first < n)
        __builtin_memcpy((char *)buf + first, p->buf, n - first);

    p->read_pos = (p->read_pos + n) % PIPE_BUF_SIZE;
    p->count   -= n;

    /* Wake a blocked writer now that there's space. */
    if (p->writer_waiting) {
        sched_wake(p->writer_waiting);
        p->writer_waiting = NULL;
    }

    return (int)n;
}

/*
 * pipe_write_fn — write up to len bytes from user buf into the pipe.
 *
 * buf is a USER virtual address (from sys_write after user_ptr_valid check).
 * Uses copy_from_user + staging buffer for SMAP correctness.
 *
 * Stack: staging[PIPE_BUF_SIZE] = 4060 bytes on kernel stack.
 * Call chain: sys_write -> pipe_write_fn. Total depth ~4400 bytes.
 * Kernel stack is 4 pages (16 KB) — within budget.
 * DO NOT add further large locals to this call chain.
 *
 * Returns n bytes written (partial write allowed; sys_write caller loops).
 */
static int
pipe_write_fn(void *priv, const void *buf, uint64_t len)
{
    pipe_t *p = (pipe_t *)priv;
    char staging[PIPE_BUF_SIZE];

    for (;;) {
        if (p->read_refs == 0)
            return -32;   /* EPIPE: all readers gone */
        if (p->count == PIPE_BUF_SIZE) {
            /* Block until a reader drains some data. */
            p->writer_waiting = sched_current();
            sched_block();
            continue;
        }
        break;
    }

    /* Compute how many bytes we can accept this call. */
    uint32_t avail = PIPE_BUF_SIZE - p->count;
    uint32_t n = (uint32_t)len;
    if (n > avail) n = avail;

    /* Copy n bytes from user space via staging buffer (SMAP guard). */
    copy_from_user(staging, buf, n);

    /* Ring buffer write with wrap-around */
    uint32_t first = PIPE_BUF_SIZE - p->write_pos;
    if (first > n) first = n;
    __builtin_memcpy(&p->buf[p->write_pos], staging, first);
    if (first < n)
        __builtin_memcpy(p->buf, staging + first, n - first);

    p->write_pos = (p->write_pos + n) % PIPE_BUF_SIZE;
    p->count    += n;

    /* Wake a blocked reader now that there's data. */
    if (p->reader_waiting) {
        sched_wake(p->reader_waiting);
        p->reader_waiting = NULL;
    }

    return (int)n;   /* partial write; caller must loop if n < len */
}

/*
 * pipe_read_close_fn — called when the read end of the pipe is closed.
 * Decrements read_refs. If write end is also gone, frees the pipe_t.
 * Wakes any blocked writer so it can observe read_refs == 0 and return EPIPE.
 */
static void
pipe_read_close_fn(void *priv)
{
    pipe_t *p = (pipe_t *)priv;
    p->read_refs--;
    if (p->writer_waiting) {
        sched_wake(p->writer_waiting);
        p->writer_waiting = NULL;
    }
    if (p->read_refs == 0 && p->write_refs == 0)
        kva_free_pages(p, 1);
}

/*
 * pipe_write_close_fn — called when the write end of the pipe is closed.
 * Decrements write_refs. Wakes any blocked reader so it observes
 * write_refs == 0 and returns 0 (EOF) on its next retry.
 */
static void
pipe_write_close_fn(void *priv)
{
    pipe_t *p = (pipe_t *)priv;
    p->write_refs--;
    if (p->reader_waiting) {
        sched_wake(p->reader_waiting);
        p->reader_waiting = NULL;
    }
    if (p->read_refs == 0 && p->write_refs == 0)
        kva_free_pages(p, 1);
}

/* dup hooks — increment the appropriate ref count when an fd is duplicated */

static void
pipe_dup_read_fn(void *priv)
{
    ((pipe_t *)priv)->read_refs++;
}

static void
pipe_dup_write_fn(void *priv)
{
    ((pipe_t *)priv)->write_refs++;
}

static int
pipe_stat_fn(void *priv, k_stat_t *st)
{
    pipe_t *p = (pipe_t *)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode = S_IFIFO | 0600;
    st->st_ino  = 0;    /* anonymous pipe: no inode */
    st->st_size = (int64_t)p->count;
    return 0;
}
