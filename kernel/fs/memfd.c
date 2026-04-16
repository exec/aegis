/* kernel/fs/memfd.c — anonymous shared memory file descriptors */
#include "memfd.h"
#include "fd_table.h"
#include "proc.h"
#include "kva.h"
#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include <stdint.h>

static memfd_t   s_memfds[MEMFD_MAX];
static spinlock_t memfd_lock = SPINLOCK_INIT;

/* ── Local helpers ─────────────────────────────────────────────────────── */

static void _mf_memset(void *dst, int val, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void _mf_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── VFS ops ───────────────────────────────────────────────────────────── */

static int memfd_vfs_read(void *priv, void *buf, uint64_t off, uint64_t len)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    irqflags_t fl = spin_lock_irqsave(&memfd_lock);
    memfd_t *mf = memfd_get(id);
    if (!mf) { spin_unlock_irqrestore(&memfd_lock, fl); return -9; }

    if (off >= mf->size) { spin_unlock_irqrestore(&memfd_lock, fl); return 0; }
    if (off + len > mf->size) len = mf->size - off;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        uint32_t page_idx = (uint32_t)((off + done) / 4096);
        uint32_t page_off = (uint32_t)((off + done) % 4096);
        uint32_t chunk = 4096 - page_off;
        if (chunk > len - done) chunk = (uint32_t)(len - done);

        if (page_idx < mf->page_count && mf->phys_pages[page_idx]) {
            /* Snapshot phys addr, release memfd_lock to avoid lock inversion
             * with vmm_window_lock. Single-reader: safe because refcount > 0
             * prevents page free. */
            uint64_t phys = mf->phys_pages[page_idx];
            spin_unlock_irqrestore(&memfd_lock, fl);

            uint8_t *src = (uint8_t *)vmm_window_map(phys);
            for (uint32_t i = 0; i < chunk; i++)
                dst[done + i] = src[page_off + i];
            vmm_window_unmap();

            fl = spin_lock_irqsave(&memfd_lock);
            mf = memfd_get(id);
            if (!mf) { spin_unlock_irqrestore(&memfd_lock, fl); return (int)done; }
        } else {
            for (uint32_t i = 0; i < chunk; i++)
                dst[done + i] = 0;
        }
        done += chunk;
    }

    spin_unlock_irqrestore(&memfd_lock, fl);
    return (int)done;
}

static int memfd_vfs_write(void *priv, const void *buf, uint64_t len)
{
    /* memfd write is not commonly used (mmap is preferred).
     * For simplicity, only support writing at offset 0 up to size. */
    (void)priv; (void)buf; (void)len;
    return -38;  /* ENOSYS — use mmap instead */
}

static void memfd_vfs_close(void *priv)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    irqflags_t fl = spin_lock_irqsave(&memfd_lock);
    if (id >= MEMFD_MAX || !s_memfds[id].in_use) {
        spin_unlock_irqrestore(&memfd_lock, fl);
        return;
    }
    memfd_t *mf = &s_memfds[id];
    if (--mf->refcount > 0) {
        spin_unlock_irqrestore(&memfd_lock, fl);
        return;
    }

    /* Free all physical pages */
    for (uint32_t i = 0; i < mf->page_count; i++) {
        if (mf->phys_pages[i])
            pmm_free_page(mf->phys_pages[i]);
    }
    mf->in_use = 0;
    spin_unlock_irqrestore(&memfd_lock, fl);
}

static void memfd_vfs_dup(void *priv)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    irqflags_t fl = spin_lock_irqsave(&memfd_lock);
    if (id < MEMFD_MAX && s_memfds[id].in_use)
        s_memfds[id].refcount++;
    spin_unlock_irqrestore(&memfd_lock, fl);
}

static int memfd_vfs_stat(void *priv, k_stat_t *st)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    _mf_memset(st, 0, sizeof(*st));
    irqflags_t fl = spin_lock_irqsave(&memfd_lock);
    memfd_t *mf = memfd_get(id);
    if (mf) {
        st->st_size = (int64_t)mf->size;
        st->st_blocks = (int64_t)(mf->page_count * 8);  /* 512-byte units */
    }
    spin_unlock_irqrestore(&memfd_lock, fl);
    st->st_mode = 0100000U | 0600U;  /* S_IFREG | 0600 */
    st->st_blksize = 4096;
    return 0;
}

/* memfd is always ready: the existing .poll callback returns
 * POLLIN | POLLOUT immediately for every open. There's no producer
 * to wake, so get_waitq stays NULL and sys_poll falls through to
 * its permissive default. */
const vfs_ops_t g_memfd_ops = {
    .read    = memfd_vfs_read,
    .write   = memfd_vfs_write,
    .close   = memfd_vfs_close,
    .readdir = (void *)0,
    .dup     = memfd_vfs_dup,
    .stat    = memfd_vfs_stat,
    .poll    = (void *)0,
};

/* ── Alloc / Get ───────────────────────────────────────────────────────── */

int memfd_alloc(const char *name)
{
    irqflags_t fl = spin_lock_irqsave(&memfd_lock);
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (!s_memfds[i].in_use) {
            _mf_memset(&s_memfds[i], 0, sizeof(memfd_t));
            s_memfds[i].in_use   = 1;
            s_memfds[i].refcount = 1;
            if (name)
                _mf_strcpy(s_memfds[i].name, name, 32);
            spin_unlock_irqrestore(&memfd_lock, fl);
            return i;
        }
    }
    spin_unlock_irqrestore(&memfd_lock, fl);
    return -1;
}

memfd_t *memfd_get(uint32_t id)
{
    if (id >= MEMFD_MAX) return (void *)0;
    if (!s_memfds[id].in_use) return (void *)0;
    return &s_memfds[id];
}

memfd_t *memfd_from_fd(int fd, void *proc_ptr)
{
    aegis_process_t *proc = (aegis_process_t *)proc_ptr;
    if (fd < 0 || fd >= PROC_MAX_FDS) return (void *)0;
    if (proc->fd_table->fds[fd].ops != &g_memfd_ops) return (void *)0;
    uint32_t id = (uint32_t)(uintptr_t)proc->fd_table->fds[fd].priv;
    return memfd_get(id);
}

/* ── Truncate ──────────────────────────────────────────────────────────── */

int memfd_truncate(uint32_t id, uint64_t size)
{
    irqflags_t fl = spin_lock_irqsave(&memfd_lock);
    memfd_t *mf = memfd_get(id);
    if (!mf) { spin_unlock_irqrestore(&memfd_lock, fl); return -9; }

    uint32_t new_pages = (uint32_t)((size + 4095) / 4096);
    if (new_pages > MEMFD_PAGES_MAX) {
        spin_unlock_irqrestore(&memfd_lock, fl);
        return -12;  /* ENOMEM */
    }

    /* Grow: allocate new pages.
     * Release memfd_lock before calling pmm/vmm to avoid lock inversion. */
    uint32_t old_count = mf->page_count;
    spin_unlock_irqrestore(&memfd_lock, fl);

    for (uint32_t i = old_count; i < new_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -12;  /* ENOMEM */

        /* Zero the page via window map */
        uint8_t *p = (uint8_t *)vmm_window_map(phys);
        for (int j = 0; j < 4096; j++) p[j] = 0;
        vmm_window_unmap();

        fl = spin_lock_irqsave(&memfd_lock);
        mf = memfd_get(id);
        if (!mf) { spin_unlock_irqrestore(&memfd_lock, fl); pmm_free_page(phys); return -9; }
        mf->phys_pages[i] = phys;
        spin_unlock_irqrestore(&memfd_lock, fl);
    }

    fl = spin_lock_irqsave(&memfd_lock);
    mf = memfd_get(id);
    if (!mf) { spin_unlock_irqrestore(&memfd_lock, fl); return -9; }

    /* Shrink: free excess pages */
    for (uint32_t i = new_pages; i < mf->page_count; i++) {
        if (mf->phys_pages[i]) {
            pmm_free_page(mf->phys_pages[i]);
            mf->phys_pages[i] = 0;
        }
    }

    mf->page_count = new_pages;
    mf->size       = size;
    spin_unlock_irqrestore(&memfd_lock, fl);
    return 0;
}

/* ── Open fd ───────────────────────────────────────────────────────────── */

int memfd_open_fd(uint32_t id, void *proc_ptr)
{
    aegis_process_t *proc = (aegis_process_t *)proc_ptr;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!proc->fd_table->fds[i].ops) {
            proc->fd_table->fds[i].ops    = &g_memfd_ops;
            proc->fd_table->fds[i].priv   = (void *)(uintptr_t)id;
            proc->fd_table->fds[i].offset = 0;
            proc->fd_table->fds[i].size   = 0;
            proc->fd_table->fds[i].flags  = 0;
            return i;
        }
    }
    return -1;
}
