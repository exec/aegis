/* ext2_internal.h — internal header for ext2 TU communication.
 * Not exported beyond kernel/fs/. Each ext2_*.c includes this. */
#pragma once

#include "ext2.h"
#include "blkdev.h"
#include "../core/printk.h"
#include "../core/spinlock.h"

/* Errno codes used within the ext2 subsystem.
 * The kernel does not include <errno.h>; define what we need here. */
#ifndef EIO
#define EIO          5
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif
#ifndef EACCES
#define EACCES       13
#endif
#ifndef EINVAL
#define EINVAL       22
#endif
#ifndef ELOOP
#define ELOOP        40
#endif

/* ── Shared state — defined in ext2.c ──────────────────────────────────── */
extern blkdev_t *s_dev;
extern ext2_superblock_t s_sb;
extern uint32_t s_block_size;
extern uint32_t s_num_groups;
extern ext2_bgd_t s_bgd[32];
extern int s_mounted;
extern spinlock_t ext2_lock;

/* ── Block cache — defined in ext2_cache.c ─────────────────────────────── */
#define CACHE_SLOTS 16

typedef struct {
    uint32_t block_num;
    uint8_t  dirty;
    uint32_t age;
    uint8_t  data[4096];    /* max block size */
} cache_slot_t;

extern cache_slot_t s_cache[CACHE_SLOTS];
extern uint32_t s_cache_age;

/* Cache functions */
int     cache_find(uint32_t block_num);
int     cache_evict(void);
void    cache_mark_dirty(uint32_t block_num);
uint8_t *cache_get_slot(uint32_t block_num);

/* ── Inode + block helpers — defined in ext2.c ─────────────────────────── */
int      ext2_read_inode(uint32_t ino, ext2_inode_t *out);
int      ext2_write_inode(uint32_t ino, const ext2_inode_t *inode);
uint32_t ext2_block_num(const ext2_inode_t *inode, uint32_t file_block);
uint32_t ext2_alloc_block(uint32_t preferred_group);
uint32_t ext2_alloc_inode(uint32_t preferred_group);

/* ── Directory helpers — defined in ext2_dir.c ─────────────────────────── */
int ext2_lookup_parent(const char *path, uint32_t *parent_ino_out,
                       const char **basename_out);
int ext2_dir_add_entry(uint32_t dir_ino, uint32_t child_ino,
                       const char *name, uint8_t file_type);
int ext2_dir_remove_entry(uint32_t dir_ino, const char *name);
