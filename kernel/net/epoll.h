/* kernel/net/epoll.h — epoll implementation */
#ifndef EPOLL_H
#define EPOLL_H

#include "sched.h"
#include "vfs.h"
#include "proc.h"
#include <stdint.h>

#define EPOLL_MAX_INSTANCES  8
#define EPOLL_MAX_WATCHES    64
#define EPOLL_NONE           0xFFFFFFFFU

/* Linux epoll event flags */
#define EPOLLIN   0x00000001U
#define EPOLLOUT  0x00000004U
#define EPOLLERR  0x00000008U
#define EPOLLHUP  0x00000010U
#define EPOLLET   0x80000000U   /* edge-triggered */

/* EPOLL_CTL opcodes */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef struct {
    uint32_t events;
    uint64_t data;   /* user data (epoll_data_t union — we treat as uint64_t) */
} k_epoll_event_t;

typedef struct {
    uint32_t fd;
    uint32_t events;
    uint64_t data;
    uint8_t  in_use;
} epoll_watch_t;

typedef struct {
    uint8_t        in_use;
    epoll_watch_t  watches[EPOLL_MAX_WATCHES];
    uint8_t        nwatches;
    aegis_task_t  *waiter_task;
    uint32_t       ready[EPOLL_MAX_WATCHES];   /* fd indices that are ready */
    uint8_t        nready;
} epoll_fd_t;

/* epoll_alloc: allocate an epoll instance. Returns epoll_id >= 0 or -1. */
int epoll_alloc(void);

/* epoll_get: return pointer to epoll_fd_t, or NULL if invalid. */
epoll_fd_t *epoll_get(uint32_t epoll_id);

/* epoll_free: release an epoll instance. */
void epoll_free(uint32_t epoll_id);

/* epoll_ctl_impl: add/del/mod a watch. */
int epoll_ctl_impl(uint32_t epoll_id, int op, int fd, k_epoll_event_t *ev);

/* epoll_notify: called from TCP/UDP when data or connection event occurs. */
void epoll_notify(uint32_t sock_id, uint32_t events);

/* epoll_wait_impl: wait for events. timeout_ticks: 0=non-blocking, UINT32_MAX=infinite. */
int epoll_wait_impl(uint32_t epoll_id, uint64_t events_uptr,
                    int maxevents, uint32_t timeout_ticks);

/* epoll_open_fd: create an fd for this epoll instance in proc->fd_table->fds[]. */
int epoll_open_fd(uint32_t epoll_id, aegis_process_t *proc);

/* epoll_id_from_fd: reverse-look up epoll_id from a fd. Returns EPOLL_NONE on error. */
uint32_t epoll_id_from_fd(int fd, aegis_process_t *proc);

#endif /* EPOLL_H */
