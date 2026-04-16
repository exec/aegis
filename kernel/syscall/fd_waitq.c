/* fd_waitq.c — fd → waitq dispatch. See fd_waitq.h. */
#include "fd_waitq.h"
#include "socket.h"
#include "unix_socket.h"
#include "proc.h"
#include "sched.h"
#include "waitq.h"
#include "vfs.h"

/* Forward decl — implemented in kernel/net/socket.c (stub returning NULL
 * until Task 6 embeds a waitq in sock_t). */
struct waitq *sock_get_waitq(uint32_t id);

struct waitq *
fd_get_waitq(int fd)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* AF_INET socket — wired in Task 6; stub returns NULL until then. */
    uint32_t sid = sock_id_from_fd(fd, proc);
    if (sid != SOCK_NONE)
        return sock_get_waitq(sid);

    /* AF_UNIX socket. */
    uint32_t uid = unix_sock_id_from_fd(fd, proc);
    if (uid != UNIX_NONE)
        return unix_sock_get_waitq(uid);

    /* VFS fd — pipe, tty, console, kbd, mouse, memfd. Each type's
     * vfs_ops_t may set get_waitq; NULL means no events to wait on. */
    if (fd >= 0 && (uint32_t)fd < PROC_MAX_FDS) {
        const vfs_ops_t *ops = proc->fd_table->fds[fd].ops;
        if (ops && ops->get_waitq)
            return ops->get_waitq(proc->fd_table->fds[fd].priv);
    }

    return (struct waitq *)0;
}
