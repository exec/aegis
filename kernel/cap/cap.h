#ifndef CAP_H
#define CAP_H

#include <stdint.h>

/* cap_slot_t — one entry in a per-process capability table.
 * kind == CAP_KIND_NULL means the slot is empty.
 * Laid out as #[repr(C)] in Rust; size = 8 bytes. */
typedef struct {
    uint32_t kind;    /* CAP_KIND_* */
    uint32_t rights;  /* CAP_RIGHTS_* bitfield */
} cap_slot_t;

#define CAP_TABLE_SIZE   16u

/* Capability kinds */
#define CAP_KIND_NULL      0u   /* empty slot */
#define CAP_KIND_VFS_OPEN  1u   /* permission to call sys_open */
#define CAP_KIND_VFS_WRITE 2u   /* permission to call sys_write */
#define CAP_KIND_VFS_READ  3u   /* permission to call sys_read */
#define CAP_KIND_AUTH      4u   /* may open /etc/shadow for reading */
#define CAP_KIND_CAP_GRANT 5u   /* may delegate caps to child processes (reserved) */
#define CAP_KIND_SETUID    6u   /* may call sys_setuid / sys_setgid */
#define CAP_KIND_NET_SOCKET 7u   /* may call sys_socket / socket syscalls */
#define CAP_KIND_NET_ADMIN  8u   /* may call sys_netcfg (set IP/mask/gw) */
#define CAP_KIND_THREAD_CREATE 9u /* may call clone(CLONE_VM) */
#define CAP_KIND_PROC_READ  10u  /* may read /proc/[other-pid] */

/* Capability rights (bitfield) */
#define CAP_RIGHTS_READ   (1u << 0)
#define CAP_RIGHTS_WRITE  (1u << 1)
#define CAP_RIGHTS_EXEC   (1u << 2)

/* ENOCAP — Aegis-specific error: no matching capability found.
 * Value 130 is outside the range of Linux errnos used in this kernel.
 * Defined without the 'u' suffix so that -ENOCAP is an unambiguous signed
 * expression; callers should use `>= 0` to check for success rather than
 * comparing to -ENOCAP to avoid signed/unsigned comparison warnings. */
#define ENOCAP 130

/* cap_init — initialize the capability subsystem.
 * Prints [CAP] OK line. Called from kernel_main before sched_init. */
void cap_init(void);

/* cap_grant — write (kind, rights) into the first empty slot of table[0..n).
 * Returns the slot index on success.
 * Returns -ENOCAP if all slots are occupied. */
int cap_grant(cap_slot_t *table, uint32_t n, uint32_t kind, uint32_t rights);

/* cap_check — return 0 if table[0..n) contains a slot with matching kind
 * and at least the requested rights; return -ENOCAP otherwise. */
int cap_check(const cap_slot_t *table, uint32_t n, uint32_t kind, uint32_t rights);

#endif /* CAP_H */
