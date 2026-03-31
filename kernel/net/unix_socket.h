/* kernel/net/unix_socket.h — AF_UNIX domain sockets */
#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#include "vfs.h"
#include "sched.h"
#include "../core/spinlock.h"
#include <stdint.h>

#define AF_UNIX          1
#define UNIX_SOCK_MAX    32
#define UNIX_PATH_MAX    108
#define UNIX_BUF_SIZE    4056   /* matches pipe ring size, fits in one kva page */
#define UNIX_NONE        0xFFFFFFFFU

/* Staged fd for SCM_RIGHTS passing */
typedef struct {
    const vfs_ops_t *ops;
    void            *priv;
    uint32_t         flags;
} unix_passed_fd_t;

#define UNIX_PASSED_FD_MAX  16

typedef enum {
    UNIX_FREE, UNIX_CREATED, UNIX_BOUND, UNIX_LISTENING,
    UNIX_CONNECTING, UNIX_CONNECTED, UNIX_CLOSED
} unix_state_t;

typedef struct {
    uint8_t        in_use;
    unix_state_t   state;
    uint8_t        nonblocking;
    char           path[UNIX_PATH_MAX];

    /* Ring buffer — this socket's tx direction (peer reads from it) */
    uint8_t       *ring;           /* kva-allocated page */
    uint16_t       ring_head;      /* write position */
    uint16_t       ring_tail;      /* read position */

    /* Peer link */
    uint32_t       peer_id;        /* connected peer (UNIX_NONE = none) */

    /* Accept queue (listening sockets) */
    uint32_t       accept_queue[8];
    uint8_t        accept_head, accept_tail;

    /* Pending connections waiting for accept */
    aegis_task_t  *connect_waiter; /* task blocked in connect() */

    /* Blocking */
    aegis_task_t  *waiter_task;    /* task blocked on read/accept */

    /* Peer credentials (captured at connect time) */
    uint32_t       peer_pid;
    uint32_t       peer_uid;
    uint32_t       peer_gid;

    /* fd passing staging area */
    unix_passed_fd_t passed_fds[UNIX_PASSED_FD_MAX];
    uint8_t          passed_fd_count;

    /* Refcount for dup/fork */
    uint32_t       refcount;
} unix_sock_t;

/* VFS ops for AF_UNIX socket fds */
extern const vfs_ops_t g_unix_sock_ops;

/* Allocate a unix socket. Returns slot index or -1. */
int unix_sock_alloc(void);

/* Get unix_sock_t by id. Returns NULL if invalid/free. */
unix_sock_t *unix_sock_get(uint32_t id);

/* Free (decrement refcount, cleanup on zero). */
void unix_sock_free(uint32_t id);

/* Wake the task blocked on this unix socket. */
void unix_sock_wake(uint32_t id);

/* Bind a unix socket to a path. Returns 0 or -errno. */
int unix_sock_bind(uint32_t id, const char *path);

/* Listen on a bound unix socket. Returns 0 or -errno. */
int unix_sock_listen(uint32_t id);

/* Connect to a listening unix socket by path. Returns 0 or -errno. */
int unix_sock_connect(uint32_t id, const char *path);

/* Accept a connection on a listening socket. Returns new sock_id or -errno. */
int unix_sock_accept(uint32_t id);

/* Read from unix socket (from peer's tx_ring). Returns bytes read or -errno. */
int unix_sock_read(uint32_t id, void *buf, uint32_t len);

/* Write to unix socket (to own tx_ring). Returns bytes written or -errno. */
int unix_sock_write(uint32_t id, const void *buf, uint32_t len);

/* Get peer credentials. Returns 0 or -errno. */
int unix_sock_peercred(uint32_t id, uint32_t *pid, uint32_t *uid, uint32_t *gid);

/* Stage fds for SCM_RIGHTS passing to peer. Returns 0 or -errno. */
int unix_sock_stage_fds(uint32_t peer_id, unix_passed_fd_t *fds, uint8_t count);

/* Receive staged fds. Returns count installed or -errno. */
int unix_sock_recv_fds(uint32_t id, int *fd_out, int max_fds);

/* Open an fd backed by a unix socket. Returns fd or -1. */
int unix_sock_open_fd(uint32_t sock_id, void *proc);

/* Get unix_sock_id from an fd. Returns UNIX_NONE if not a unix socket. */
uint32_t unix_sock_id_from_fd(int fd, void *proc);

#endif /* UNIX_SOCKET_H */
