/* kernel/fs/pty.h -- pseudo-terminal pair management */
#ifndef AEGIS_PTY_H
#define AEGIS_PTY_H

#include "vfs.h"
#include "tty.h"
#include <stdint.h>

#define PTY_MAX_PAIRS  16
#define PTY_BUF_SIZE   4096

typedef struct {
    uint8_t  input_buf[PTY_BUF_SIZE];    /* master->slave ring buffer */
    uint32_t input_head, input_tail;
    uint8_t  output_buf[PTY_BUF_SIZE];   /* slave->master ring buffer */
    uint32_t output_head, output_tail;
    tty_t    tty;                         /* slave's TTY */
    uint8_t  master_open;
    uint8_t  master_refs;                 /* refcount for master fd (dup/fork) */
    uint8_t  slave_open;
    uint8_t  slave_refs;                  /* refcount for slave fd */
    uint8_t  locked;                      /* cleared by unlockpt */
    uint8_t  in_use;
    uint8_t  index;                       /* 0-15 */
} pty_pair_t;

/* ptmx_open -- allocate a PTY pair and return the master fd.
 * Returns 0 on success, -12 (ENOMEM) if pool exhausted. */
int ptmx_open(int flags, vfs_file_t *out);

/* pts_open -- open the slave side of PTY pair N.
 * Returns 0 on success, -2 (ENOENT) if not allocated, -13 (EACCES) if locked. */
int pts_open(uint32_t index, int flags, vfs_file_t *out);

/* pty_find_by_session -- find a PTY whose tty.session_id matches.
 * Returns pointer to tty_t, or NULL. */
tty_t *pty_find_by_session(uint32_t session_id);

/* pty_is_master -- returns 1 if the vfs_file is a PTY master fd. */
int pty_is_master(const vfs_file_t *f);

/* pty_is_slave -- returns 1 if the vfs_file is a PTY slave fd. */
int pty_is_slave(const vfs_file_t *f);

/* pty_get_tty -- if the fd is a PTY slave, return its tty_t. Else NULL. */
tty_t *pty_get_tty(const vfs_file_t *f);

#endif /* AEGIS_PTY_H */
