/* ext2.c — ext2 filesystem driver (mount, inode ops, block alloc, public API)
 *
 * Block cache lives in ext2_cache.c.
 * Directory helpers live in ext2_dir.c.
 * Shared state and internal declarations in ext2_internal.h.
 *
 * No libc, no malloc, no VLAs.
 */

#include "ext2_internal.h"

/* ------------------------------------------------------------------ */
/* Shared globals — accessed by ext2_cache.c and ext2_dir.c via extern */
/* ------------------------------------------------------------------ */

blkdev_t *s_dev;
ext2_superblock_t s_sb;
uint32_t s_block_size;
uint32_t s_num_groups;
ext2_bgd_t s_bgd[32];   /* support up to 32 block groups */
int s_mounted = 0;

/* ------------------------------------------------------------------ */
/* ext2_mount                                                          */
/* ------------------------------------------------------------------ */

int ext2_mount(const char *devname)
{
    uint32_t i;

    /* Initialise cache slots to age 0 (unused) */
    for (i = 0; i < CACHE_SLOTS; i++) {
        s_cache[i].block_num = 0;
        s_cache[i].dirty = 0;
        s_cache[i].age = 0;
    }

    s_dev = blkdev_get(devname);
    if (!s_dev)
        return -1;  /* silent — no NVMe on -machine pc */

    /* Superblock is at byte offset 1024 from partition start.
     * blkdev uses 512-byte sectors: LBA 2 = byte 1024. */
    uint8_t sb_buf[1024];
    if (s_dev->read(s_dev, 2, 2, sb_buf) < 0)
        return -1;

    /* Copy superblock from start of buffer */
    uint8_t *src = sb_buf;
    uint8_t *dst = (uint8_t *)&s_sb;
    for (i = 0; i < sizeof(ext2_superblock_t); i++)
        dst[i] = src[i];

    if (s_sb.s_magic != EXT2_MAGIC)
        return -1;

    if (s_sb.s_log_block_size > 6) {   /* ext2 max block size is 64KB (2^6 * 1024) */
        printk("[EXT2] FAIL: invalid log_block_size %u\n",
               (unsigned)s_sb.s_log_block_size);
        return -1;
    }
    s_block_size = 1024u << s_sb.s_log_block_size;
    if (s_sb.s_rev_level >= 1 && (s_sb.s_inode_size < 128 || s_sb.s_inode_size > 4096)) {
        return -1;
    }
    s_num_groups = (s_sb.s_blocks_count + s_sb.s_blocks_per_group - 1)
                   / s_sb.s_blocks_per_group;
    if (s_num_groups > 32)
        s_num_groups = 32;

    /* BGD table is at the block immediately after the superblock.
     * For 1024-byte blocks: superblock is in block 1, BGD at block 2.
     * For larger blocks:    superblock is in block 0, BGD at block 1. */
    uint32_t bgd_block = (s_sb.s_first_data_block == 1) ? 2 : 1;

    uint8_t *bgd_buf = cache_get_slot(bgd_block);
    if (!bgd_buf)
        return -1;

    uint32_t bgd_bytes = s_num_groups * sizeof(ext2_bgd_t);
    src = bgd_buf;
    dst = (uint8_t *)s_bgd;
    for (i = 0; i < bgd_bytes; i++)
        dst[i] = src[i];

    s_mounted = 1;
    printk("[EXT2] OK: mounted %s, %u blocks, %u inodes\n",
           devname, s_sb.s_blocks_count, s_sb.s_inodes_count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_read_inode (internal)                                          */
/* ------------------------------------------------------------------ */

int ext2_read_inode(uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0) return -EIO;   /* inode 0 is reserved/invalid in ext2 */
    uint32_t group       = (ino - 1) / s_sb.s_inodes_per_group;
    uint32_t index       = (ino - 1) % s_sb.s_inodes_per_group;
    if (group >= s_num_groups || group >= 32) {
        return -1;
    }
    uint32_t inode_size  = (s_sb.s_rev_level >= 1)
                           ? (uint32_t)s_sb.s_inode_size : 128u;
    uint32_t inode_table_block = s_bgd[group].bg_inode_table;
    uint32_t byte_offset = index * inode_size;
    uint32_t block_offset = byte_offset / s_block_size;
    uint32_t in_block    = byte_offset % s_block_size;

    uint8_t *data = cache_get_slot(inode_table_block + block_offset);
    if (!data)
        return -1;

    uint8_t *src = data + in_block;
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;
    for (i = 0; i < sizeof(ext2_inode_t); i++)
        dst[i] = src[i];

    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_write_inode (internal)                                         */
/* ------------------------------------------------------------------ */

int ext2_write_inode(uint32_t ino, const ext2_inode_t *inode)
{
    uint32_t group       = (ino - 1) / s_sb.s_inodes_per_group;
    uint32_t index       = (ino - 1) % s_sb.s_inodes_per_group;
    if (group >= s_num_groups || group >= 32) {
        return -1;
    }
    uint32_t inode_size  = (s_sb.s_rev_level >= 1)
                           ? (uint32_t)s_sb.s_inode_size : 128u;
    uint32_t inode_table_block = s_bgd[group].bg_inode_table;
    uint32_t byte_offset = index * inode_size;
    uint32_t block_offset = byte_offset / s_block_size;
    uint32_t in_block    = byte_offset % s_block_size;

    uint8_t *data = cache_get_slot(inode_table_block + block_offset);
    if (!data)
        return -1;

    uint8_t *dst = data + in_block;
    const uint8_t *src = (const uint8_t *)inode;
    uint32_t i;
    for (i = 0; i < sizeof(ext2_inode_t); i++)
        dst[i] = src[i];

    cache_mark_dirty(inode_table_block + block_offset);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_block_num (internal)                                           */
/* ------------------------------------------------------------------ */

uint32_t ext2_block_num(const ext2_inode_t *inode,
                        uint32_t file_block)
{
    uint32_t ptrs_per_block = s_block_size / 4;

    if (file_block < 12)
        return inode->i_block[file_block];

    if (file_block < 12 + ptrs_per_block) {
        uint32_t indirect = inode->i_block[12];
        if (indirect == 0)
            return 0;
        uint8_t *data = cache_get_slot(indirect);
        if (!data)
            return 0;
        uint32_t off = file_block - 12;
        uint32_t entry;
        uint8_t *p = data + off * 4;
        entry  = (uint32_t)p[0];
        entry |= (uint32_t)p[1] << 8;
        entry |= (uint32_t)p[2] << 16;
        entry |= (uint32_t)p[3] << 24;
        return entry;
    }

    return 0;   /* double/triple indirect not supported */
}

/* ------------------------------------------------------------------ */
/* ext2_open — walk path from root inode 2                             */
/* ------------------------------------------------------------------ */

int ext2_open(const char *path, uint32_t *inode_out)
{
    if (!s_mounted)
        return -1;

    uint32_t current_ino = EXT2_ROOT_INODE;

    /* skip leading slashes */
    while (*path == '/')
        path++;

    /* If path is empty (root dir itself) */
    if (*path == '\0') {
        *inode_out = current_ino;
        return 0;
    }

    while (*path != '\0') {
        /* Extract next component into a local buffer */
        char component[256];
        uint32_t clen = 0;
        while (*path != '\0' && *path != '/') {
            if (clen < 255)
                component[clen++] = *path;
            path++;
        }
        component[clen] = '\0';

        /* Skip trailing slashes between components */
        while (*path == '/')
            path++;

        /* Prevent directory escape via ".." — clamp to filesystem root. */
        if (component[0] == '.' && component[1] == '.' && component[2] == '\0') {
            current_ino = EXT2_ROOT_INODE;
            continue;
        }

        /* Read current directory inode */
        ext2_inode_t inode;
        if (ext2_read_inode(current_ino, &inode) < 0)
            return -1;

        /* Must be a directory */
        if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
            return -1;

        /* Search directory entries for component */
        int found = 0;
        uint32_t pos = 0;

        while (pos < inode.i_size) {
            uint32_t file_block = pos / s_block_size;
            uint32_t blk = ext2_block_num(&inode, file_block);
            if (blk == 0)
                break;
            uint8_t *data = cache_get_slot(blk);
            if (!data)
                return -1;
            uint32_t block_pos = pos % s_block_size;
            while (block_pos < s_block_size) {
                ext2_dirent_t *de =
                    (ext2_dirent_t *)(data + block_pos);
                if (de->rec_len < 8 || block_pos + de->rec_len > s_block_size)
                    break;
                if (de->inode == 0) {
                    block_pos += de->rec_len;
                    pos += de->rec_len;
                    continue;
                }
                if (de->name_len == (uint8_t)clen) {
                    /* manual name compare */
                    uint32_t k;
                    int match = 1;
                    for (k = 0; k < clen; k++) {
                        if (de->name[k] != component[k]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        current_ino = de->inode;
                        found = 1;
                        break;
                    }
                }
                block_pos += de->rec_len;
                pos += de->rec_len;
            }
            if (found)
                break;
            /* Advance to next block if we didn't break out */
            if (!found) {
                uint32_t block_end = (file_block + 1) * s_block_size;
                if (pos < block_end)
                    pos = block_end;
            }
        }

        if (!found)
            return -1;
    }

    *inode_out = current_ino;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_read                                                           */
/* ------------------------------------------------------------------ */

int ext2_read(uint32_t inode_num, void *buf, uint32_t offset, uint32_t len)
{
    if (!s_mounted)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) < 0)
        return -1;

    /* S9: Reject unreasonably large inodes from malicious ext2 images. */
    if (inode.i_size > (256U * 1024U * 1024U))  /* 256 MB cap */
        return -5;  /* -EIO */

    if (offset >= inode.i_size)
        return 0;

    if (offset + len > inode.i_size)
        len = inode.i_size - offset;

    uint8_t *out = (uint8_t *)buf;
    uint32_t bytes_read = 0;

    while (bytes_read < len) {
        uint32_t cur_off = offset + bytes_read;
        uint32_t file_block = cur_off / s_block_size;
        uint32_t in_block   = cur_off % s_block_size;
        uint32_t can_copy   = s_block_size - in_block;
        if (can_copy > len - bytes_read)
            can_copy = len - bytes_read;

        uint32_t blk = ext2_block_num(&inode, file_block);
        if (blk == 0) {
            /* sparse block — fill zeros */
            uint32_t i;
            for (i = 0; i < can_copy; i++)
                out[bytes_read + i] = 0;
        } else {
            uint8_t *data = cache_get_slot(blk);
            if (!data)
                return (int)bytes_read;
            uint32_t i;
            for (i = 0; i < can_copy; i++)
                out[bytes_read + i] = data[in_block + i];
        }
        bytes_read += can_copy;
    }

    return (int)bytes_read;
}

/* ------------------------------------------------------------------ */
/* ext2_file_size                                                       */
/* ------------------------------------------------------------------ */

int ext2_file_size(uint32_t inode_num)
{
    if (!s_mounted)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) < 0)
        return -1;

    return (int)inode.i_size;
}

/* ------------------------------------------------------------------ */
/* ext2_readdir — index-based directory iteration                      */
/* ------------------------------------------------------------------ */

int ext2_readdir(uint32_t dir_inode, uint64_t index,
                 char *name_out, uint8_t *type_out)
{
    if (!s_mounted) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(dir_inode, &inode) < 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint64_t count = 0;
    uint32_t file_block_idx = 0;
    uint32_t bytes_walked = 0;

    while (bytes_walked < inode.i_size) {
        uint32_t blk = ext2_block_num(&inode, file_block_idx);
        if (blk == 0) {
            bytes_walked += s_block_size;
            file_block_idx++;
            continue;
        }
        uint8_t *data = cache_get_slot(blk);
        if (!data) return -1;
        uint32_t block_pos = 0;
        while (block_pos + 8 <= s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(data + block_pos);
            if (de->rec_len < 8 || block_pos + de->rec_len > s_block_size)
                break;
            if (de->inode != 0) {
                if (count == index) {
                    uint8_t nlen = de->name_len;
                    uint32_t k;
                    for (k = 0; k < nlen; k++) name_out[k] = de->name[k];
                    name_out[nlen] = '\0';
                    *type_out = (de->file_type == EXT2_FT_DIR) ? 4u : 8u;
                    return 0;
                }
                count++;
            }
            block_pos += de->rec_len;
        }
        bytes_walked += s_block_size;
        file_block_idx++;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ext2_is_dir — returns 1 if inode is a directory, 0 otherwise       */
/* ------------------------------------------------------------------ */

int ext2_is_dir(uint32_t ino)
{
    if (!s_mounted) return 0;
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return 0;
    return ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Block and inode allocation                                          */
/* ------------------------------------------------------------------ */

/* ext2_alloc_block — scan block bitmaps for a free bit.
 * preferred_group: start scanning from this group (locality hint).
 * Returns allocated block number, or 0 on failure. */
uint32_t ext2_alloc_block(uint32_t preferred_group)
{
    uint32_t g, i;
    for (g = 0; g < s_num_groups; g++) {
        uint32_t grp = (g + preferred_group) % s_num_groups;
        if (s_bgd[grp].bg_free_blocks_count == 0)
            continue;
        uint8_t *bitmap = cache_get_slot(s_bgd[grp].bg_block_bitmap);
        if (!bitmap)
            continue;
        uint32_t blocks_in_group = (grp == s_num_groups - 1)
            ? (s_sb.s_blocks_count - grp * s_sb.s_blocks_per_group)
            : s_sb.s_blocks_per_group;
        for (i = 0; i < blocks_in_group; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                bitmap[i / 8] |= (1u << (i % 8));
                cache_mark_dirty(s_bgd[grp].bg_block_bitmap);
                s_bgd[grp].bg_free_blocks_count--;
                s_sb.s_free_blocks_count--;
                return grp * s_sb.s_blocks_per_group + i + s_sb.s_first_data_block;
            }
        }
    }
    return 0; /* no free block */
}

/* ext2_alloc_inode — scan inode bitmaps for a free bit.
 * preferred_group: start scanning from this group.
 * Returns allocated inode number (1-based), or 0 on failure. */
uint32_t ext2_alloc_inode(uint32_t preferred_group)
{
    uint32_t g, i;
    for (g = 0; g < s_num_groups; g++) {
        uint32_t grp = (g + preferred_group) % s_num_groups;
        if (s_bgd[grp].bg_free_inodes_count == 0)
            continue;
        uint8_t *bitmap = cache_get_slot(s_bgd[grp].bg_inode_bitmap);
        if (!bitmap)
            continue;
        for (i = 0; i < s_sb.s_inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                bitmap[i / 8] |= (1u << (i % 8));
                cache_mark_dirty(s_bgd[grp].bg_inode_bitmap);
                s_bgd[grp].bg_free_inodes_count--;
                s_sb.s_free_inodes_count--;
                return grp * s_sb.s_inodes_per_group + i + 1; /* 1-based */
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Write path                                                          */
/* ------------------------------------------------------------------ */

int ext2_write(uint32_t inode_num, const void *buf,
               uint32_t offset, uint32_t len)
{
    if (!s_mounted || len == 0)
        return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0)
        return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bytes_written = 0;
    uint32_t orig_size = inode.i_size;   /* save before write loop */

    while (bytes_written < len) {
        uint32_t cur_offset = offset + bytes_written;
        uint32_t file_block = cur_offset / s_block_size;
        uint32_t in_block   = cur_offset % s_block_size;
        uint32_t can_write  = s_block_size - in_block;
        if (can_write > len - bytes_written)
            can_write = len - bytes_written;

        uint32_t blk = ext2_block_num(&inode, file_block);
        if (blk == 0) {
            /* allocate a new direct block (indirect not yet supported) */
            if (file_block < 12) {
                blk = ext2_alloc_block(0);
                if (blk == 0) {
                    /* Partial write: commit what was written so far */
                    uint32_t actual_end = offset + bytes_written;
                    inode.i_size = (actual_end > orig_size) ? actual_end : orig_size;
                    inode.i_blocks = (inode.i_size + 511u) / 512u;
                    ext2_write_inode(inode_num, &inode);
                    return (bytes_written > 0) ? (int)bytes_written : -EIO;
                }
                inode.i_block[file_block] = blk;
                /* zero the new block */
                uint8_t *newdata = cache_get_slot(blk);
                if (!newdata)
                    break;
                uint32_t zi;
                for (zi = 0; zi < s_block_size; zi++)
                    newdata[zi] = 0;
                cache_mark_dirty(blk);
            } else {
                break; /* indirect not yet supported for write */
            }
        }

        uint8_t *data = cache_get_slot(blk);
        if (!data)
            break;
        uint32_t wi;
        for (wi = 0; wi < can_write; wi++)
            data[in_block + wi] = src[bytes_written + wi];
        cache_mark_dirty(blk);
        bytes_written += can_write;
    }

    /* update inode size and 512-byte sector count */
    uint32_t end = offset + bytes_written;
    if (end > inode.i_size) {
        inode.i_size = end;
        inode.i_blocks = (inode.i_size + 511u) / 512u;
    }
    ext2_write_inode(inode_num, &inode);
    return (int)bytes_written;
}

int ext2_create(const char *path, uint16_t mode)
{
    if (!s_mounted)
        return -1;

    uint32_t parent_ino;
    const char *basename;
    if (ext2_lookup_parent(path, &parent_ino, &basename) != 0) {
        printk("[EXT2] creat: lookup_parent failed for %s\n", path);
        return -1;
    }

    uint32_t new_ino = ext2_alloc_inode(0);
    if (new_ino == 0) {
        printk("[EXT2] creat: alloc_inode failed\n");
        return -1;
    }

    ext2_inode_t inode;
    uint32_t ci;
    for (ci = 0; ci < sizeof(inode); ci++)
        ((uint8_t *)&inode)[ci] = 0;
    inode.i_mode = EXT2_S_IFREG | (mode & 0x1FFu);
    inode.i_links_count = 1;
    ext2_write_inode(new_ino, &inode);
    int r = ext2_dir_add_entry(parent_ino, new_ino, basename, EXT2_FT_REG_FILE);
    if (r != 0)
        printk("[EXT2] creat: dir_add_entry failed parent=%u ino=%u\n",
               parent_ino, new_ino);
    return r;
}

int ext2_unlink(const char *path)
{
    if (!s_mounted)
        return -1;

    uint32_t parent_ino;
    const char *basename;
    if (ext2_lookup_parent(path, &parent_ino, &basename) != 0)
        return -1;

    uint32_t ino;
    if (ext2_open(path, &ino) != 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    if (inode.i_links_count > 0)
        inode.i_links_count--;

    if (inode.i_links_count == 0) {
        /* free direct data blocks */
        uint32_t bi;
        for (bi = 0; bi < 12; bi++) {
            if (inode.i_block[bi] != 0) {
                uint32_t blk = inode.i_block[bi];
                uint32_t grp = 0;
                if (s_sb.s_blocks_per_group > 0)
                    grp = (blk - s_sb.s_first_data_block) / s_sb.s_blocks_per_group;
                if (grp < s_num_groups) {
                    uint8_t *bitmap = cache_get_slot(s_bgd[grp].bg_block_bitmap);
                    if (bitmap) {
                        uint32_t bit = (blk - s_sb.s_first_data_block)
                                       % s_sb.s_blocks_per_group;
                        bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
                        cache_mark_dirty(s_bgd[grp].bg_block_bitmap);
                        s_bgd[grp].bg_free_blocks_count++;
                        s_sb.s_free_blocks_count++;
                    }
                }
                inode.i_block[bi] = 0;
            }
        }
        /* free inode */
        uint32_t igrp = 0;
        if (s_sb.s_inodes_per_group > 0)
            igrp = (ino - 1) / s_sb.s_inodes_per_group;
        if (igrp < s_num_groups) {
            uint8_t *ibitmap = cache_get_slot(s_bgd[igrp].bg_inode_bitmap);
            if (ibitmap) {
                uint32_t ibit = (ino - 1) % s_sb.s_inodes_per_group;
                ibitmap[ibit / 8] &= (uint8_t)~(1u << (ibit % 8));
                cache_mark_dirty(s_bgd[igrp].bg_inode_bitmap);
                s_bgd[igrp].bg_free_inodes_count++;
                s_sb.s_free_inodes_count++;
            }
        }
        inode.i_dtime = 1; /* mark deleted */
    }
    ext2_write_inode(ino, &inode);
    return ext2_dir_remove_entry(parent_ino, basename);
}

int ext2_mkdir(const char *path, uint16_t mode)
{
    if (!s_mounted)
        return -1;

    uint32_t parent_ino;
    const char *basename;
    if (ext2_lookup_parent(path, &parent_ino, &basename) != 0)
        return -1;

    uint32_t new_ino = ext2_alloc_inode(0);
    if (new_ino == 0)
        return -1;

    uint32_t blk = ext2_alloc_block(0);
    if (blk == 0)
        return -1;

    ext2_inode_t inode;
    uint32_t ci;
    for (ci = 0; ci < sizeof(inode); ci++)
        ((uint8_t *)&inode)[ci] = 0;
    inode.i_mode = EXT2_S_IFDIR | (mode & 0x1FFu);
    inode.i_links_count = 2; /* "." + parent's ".." */
    inode.i_size = s_block_size;
    inode.i_blocks = s_block_size / 512;
    inode.i_block[0] = blk;
    ext2_write_inode(new_ino, &inode);

    /* initialise "." and ".." entries in the new block */
    uint8_t *data = cache_get_slot(blk);
    if (!data)
        return -1;
    uint32_t zi;
    for (zi = 0; zi < s_block_size; zi++)
        data[zi] = 0;

    /* "." entry */
    ext2_dirent_t *dot = (ext2_dirent_t *)data;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';

    /* ".." entry */
    ext2_dirent_t *dotdot = (ext2_dirent_t *)(data + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(s_block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';
    cache_mark_dirty(blk);

    /* increment parent link count for ".." back-reference */
    ext2_inode_t parent;
    if (ext2_read_inode(parent_ino, &parent) == 0) {
        parent.i_links_count++;
        ext2_write_inode(parent_ino, &parent);
    }

    /* update block group directory count */
    uint32_t grp = (new_ino - 1) / s_sb.s_inodes_per_group;
    if (grp < s_num_groups)
        s_bgd[grp].bg_used_dirs_count++;

    return ext2_dir_add_entry(parent_ino, new_ino, basename, EXT2_FT_DIR);
}

int ext2_rename(const char *old_path, const char *new_path)
{
    if (!s_mounted)
        return -1;

    uint32_t ino;
    if (ext2_open(old_path, &ino) != 0)
        return -1;

    uint32_t old_parent_ino;
    const char *old_basename;
    if (ext2_lookup_parent(old_path, &old_parent_ino, &old_basename) != 0)
        return -1;

    uint32_t new_parent_ino;
    const char *new_basename;
    if (ext2_lookup_parent(new_path, &new_parent_ino, &new_basename) != 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;
    uint8_t ftype = ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
                    ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

    if (ext2_dir_remove_entry(old_parent_ino, old_basename) != 0)
        return -1;
    return ext2_dir_add_entry(new_parent_ino, ino, new_basename, ftype);
}
