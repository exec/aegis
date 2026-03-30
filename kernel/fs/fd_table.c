#include "fd_table.h"
#include "kva.h"

fd_table_t *
fd_table_alloc(void)
{
    fd_table_t *t = (fd_table_t *)kva_alloc_pages(1);
    if (!t) return (fd_table_t *)0;
    uint32_t i;
    for (i = 0; i < PROC_MAX_FDS; i++)
        t->fds[i].ops = (const vfs_ops_t *)0;
    t->refcount = 1;
    return t;
}

void
fd_table_ref(fd_table_t *t)
{
    if (t) __atomic_fetch_add(&t->refcount, 1, __ATOMIC_SEQ_CST);
}

void
fd_table_unref(fd_table_t *t)
{
    if (!t) return;
    if (__atomic_fetch_sub(&t->refcount, 1, __ATOMIC_SEQ_CST) > 1) return;
    uint32_t i;
    for (i = 0; i < PROC_MAX_FDS; i++) {
        if (t->fds[i].ops && t->fds[i].ops->close) {
            t->fds[i].ops->close(t->fds[i].priv);
            t->fds[i].ops = (const vfs_ops_t *)0;
        }
    }
    kva_free_pages(t, 1);
}

fd_table_t *
fd_table_copy(fd_table_t *src)
{
    if (!src) return (fd_table_t *)0;
    fd_table_t *dst = (fd_table_t *)kva_alloc_pages(1);
    if (!dst) return (fd_table_t *)0;
    uint32_t i;
    for (i = 0; i < PROC_MAX_FDS; i++)
        dst->fds[i] = src->fds[i];
    for (i = 0; i < PROC_MAX_FDS; i++) {
        if (dst->fds[i].ops && dst->fds[i].ops->dup)
            dst->fds[i].ops->dup(dst->fds[i].priv);
    }
    dst->refcount = 1;
    return dst;
}
