/* kernel/net/epoll.c — epoll implementation */
#include "epoll.h"
#include "proc.h"
#include "uaccess.h"
#include "arch.h"
#include <stdint.h>

static epoll_fd_t s_epoll[EPOLL_MAX_INSTANCES];

/* ── epoll VFS ops ───────────────────────────────────────────────────────── */

static void epoll_vfs_close(void *priv)
{
    epoll_free((uint32_t)(uintptr_t)priv);
}

static const vfs_ops_t s_epoll_ops = {
    .read    = (void *)0,
    .write   = (void *)0,
    .close   = epoll_vfs_close,
    .readdir = (void *)0,
    .dup     = (void *)0,
    .stat    = (void *)0,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

int epoll_alloc(void)
{
    uint32_t i;
    for (i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (!s_epoll[i].in_use) {
            __builtin_memset(&s_epoll[i], 0, sizeof(s_epoll[i]));
            s_epoll[i].in_use = 1;
            return (int)i;
        }
    }
    return -1;
}

epoll_fd_t *epoll_get(uint32_t epoll_id)
{
    if (epoll_id >= EPOLL_MAX_INSTANCES) return (epoll_fd_t *)0;
    if (!s_epoll[epoll_id].in_use) return (epoll_fd_t *)0;
    return &s_epoll[epoll_id];
}

void epoll_free(uint32_t epoll_id)
{
    if (epoll_id >= EPOLL_MAX_INSTANCES) return;
    s_epoll[epoll_id].in_use = 0;
}

int epoll_ctl_impl(uint32_t epoll_id, int op, int fd, k_epoll_event_t *ev)
{
    epoll_fd_t *ep = epoll_get(epoll_id);
    if (!ep) return -9;  /* EBADF */

    if (op == EPOLL_CTL_ADD) {
        if (ep->nwatches >= EPOLL_MAX_WATCHES) return -12; /* ENOMEM */
        uint8_t i;
        for (i = 0; i < EPOLL_MAX_WATCHES; i++)
            if (ep->watches[i].in_use && ep->watches[i].fd == (uint32_t)fd)
                return -17;  /* EEXIST */
        /* Find a free slot */
        for (i = 0; i < EPOLL_MAX_WATCHES; i++) {
            if (!ep->watches[i].in_use) {
                ep->watches[i].fd     = (uint32_t)fd;
                ep->watches[i].events = ev->events;
                ep->watches[i].data   = ev->data;
                ep->watches[i].in_use = 1;
                ep->nwatches++;
                return 0;
            }
        }
        return -12;
    }

    if (op == EPOLL_CTL_DEL || op == EPOLL_CTL_MOD) {
        uint8_t i;
        for (i = 0; i < EPOLL_MAX_WATCHES; i++) {
            if (ep->watches[i].in_use && ep->watches[i].fd == (uint32_t)fd) {
                if (op == EPOLL_CTL_DEL) {
                    ep->watches[i].in_use = 0;
                    ep->nwatches--;
                } else {
                    ep->watches[i].events = ev->events;
                    ep->watches[i].data   = ev->data;
                }
                return 0;
            }
        }
        return -2;  /* ENOENT */
    }

    return -22;  /* EINVAL */
}

void epoll_notify(uint32_t sock_id_as_fd, uint32_t events)
{
    uint32_t e;
    for (e = 0; e < EPOLL_MAX_INSTANCES; e++) {
        epoll_fd_t *ep = &s_epoll[e];
        if (!ep->in_use) continue;
        uint8_t i;
        for (i = 0; i < EPOLL_MAX_WATCHES; i++) {
            if (!ep->watches[i].in_use) continue;
            if (ep->watches[i].fd != sock_id_as_fd) continue;
            if (!(ep->watches[i].events & events)) continue;
            /* Mark ready — skip if already queued (dedup) */
            if (ep->nready < EPOLL_MAX_WATCHES) {
                uint8_t already = 0;
                uint8_t k;
                for (k = 0; k < ep->nready; k++) {
                    if (ep->ready[k] == i) { already = 1; break; }
                }
                if (!already)
                    ep->ready[ep->nready++] = i;
            }
            /* Wake any blocked epoll_wait */
            if (ep->waiter_task) {
                sched_wake(ep->waiter_task);
                ep->waiter_task = (aegis_task_t *)0;
            }
        }
    }
}

int epoll_wait_impl(uint32_t epoll_id, uint64_t events_uptr,
                    int maxevents, uint32_t timeout_ticks)
{
    epoll_fd_t *ep = epoll_get(epoll_id);
    if (!ep) return -9;  /* EBADF */
    if (maxevents <= 0) return -22;  /* EINVAL */

    uint32_t deadline = 0;
    uint8_t  has_deadline = 0;
    if (timeout_ticks > 0 && timeout_ticks != 0xFFFFFFFFU) {
        deadline = (uint32_t)arch_get_ticks() + timeout_ticks;
        has_deadline = 1;
    }

    for (;;) {
        if (ep->nready > 0) {
            int limit = (ep->nready < (uint8_t)maxevents) ? (int)ep->nready : maxevents;
            int delivered = 0;
            int i = 0;
            while (i < limit && delivered < maxevents) {
                uint8_t wi = (uint8_t)ep->ready[i];
                k_epoll_event_t kev;
                kev.events = ep->watches[wi].events;
                kev.data   = ep->watches[wi].data;
                /* copy_to_user returns void — no fault recovery without extable */
                copy_to_user((void *)(uintptr_t)(events_uptr +
                             (uint64_t)delivered * sizeof(k_epoll_event_t)),
                             &kev, sizeof(kev));
                delivered++;
                if (ep->watches[wi].events & EPOLLET) {
                    /* Edge-triggered: remove this entry from ready list */
                    uint8_t j;
                    for (j = (uint8_t)i; j + 1 < ep->nready; j++)
                        ep->ready[j] = ep->ready[j + 1];
                    ep->nready--;
                    /* Don't advance i — next entry slid into position i */
                    limit--;
                } else {
                    i++;  /* Level-triggered: keep in ready, advance */
                }
            }
            return delivered;
        }

        if (timeout_ticks == 0) return 0;  /* non-blocking */
        if (has_deadline && (uint32_t)arch_get_ticks() >= deadline) return 0;

        ep->waiter_task = (aegis_task_t *)sched_current();
        sched_block();
        /* Loop: check ready list again after wake */
    }
}

int epoll_open_fd(uint32_t epoll_id, aegis_process_t *proc)
{
    uint32_t fd;
    for (fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (!proc->fds[fd].ops) {
            proc->fds[fd].ops    = &s_epoll_ops;
            proc->fds[fd].priv   = (void *)(uintptr_t)epoll_id;
            proc->fds[fd].offset = 0;
            proc->fds[fd].size   = 0;
            proc->fds[fd].flags  = 0;
            return (int)fd;
        }
    }
    return -1;
}

uint32_t epoll_id_from_fd(int fd, aegis_process_t *proc)
{
    if (fd < 0 || (uint32_t)fd >= PROC_MAX_FDS) return EPOLL_NONE;
    if (proc->fds[fd].ops != &s_epoll_ops) return EPOLL_NONE;
    return (uint32_t)(uintptr_t)proc->fds[fd].priv;
}
