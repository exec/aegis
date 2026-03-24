/* ext2_cache.c — 16-slot LRU block cache and ext2_sync */
#include "ext2_internal.h"

/* Cache globals */
cache_slot_t s_cache[CACHE_SLOTS];
uint32_t s_cache_age = 0;

/* cache_find — return slot index of block_num, or -1 if not cached */
int cache_find(uint32_t block_num)
{
    int i;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].block_num == block_num && s_cache[i].age != 0)
            return i;
    }
    return -1;
}

/* cache_evict — find the LRU slot (lowest age, prefer clean over dirty).
 * Writes back dirty data before evicting. Returns slot index. */
int cache_evict(void)
{
    int i;
    int best = 0;

    /* Prefer a clean slot with the lowest age */
    for (i = 1; i < CACHE_SLOTS; i++) {
        /* Prefer clean over dirty */
        if (s_cache[i].dirty == 0 && s_cache[best].dirty != 0) {
            best = i;
            continue;
        }
        if (s_cache[i].dirty != 0 && s_cache[best].dirty == 0) {
            continue;
        }
        /* Same cleanliness: pick lower age */
        if (s_cache[i].age < s_cache[best].age) {
            best = i;
        }
    }

    /* Write back if dirty */
    if (s_cache[best].dirty && s_cache[best].block_num != 0) {
        uint64_t lba = (uint64_t)s_cache[best].block_num *
                       (s_block_size / 512);
        int wr = s_dev->write(s_dev, lba, s_block_size / 512,
                              s_cache[best].data);
        if (wr != 0) {
            printk("[EXT2] WARN: cache flush failed for block %u\n",
                   s_cache[best].block_num);
            /* Clear dirty anyway to avoid infinite eviction loop */
        }
        s_cache[best].dirty = 0;
    }

    return best;
}


/* cache_mark_dirty — mark the cached slot for block_num dirty */
void cache_mark_dirty(uint32_t block_num)
{
    int idx = cache_find(block_num);
    if (idx >= 0)
        s_cache[idx].dirty = 1;
}

/* cache_get_slot — return pointer to cached data for block_num,
 * loading from disk if necessary. Returns NULL on I/O error. */
uint8_t *cache_get_slot(uint32_t block_num)
{
    int idx = cache_find(block_num);

    if (idx < 0) {
        idx = cache_evict();
        s_cache[idx].block_num = block_num;
        s_cache[idx].dirty = 0;
        s_cache_age++;
        s_cache[idx].age = s_cache_age;

        uint64_t lba = (uint64_t)block_num * (s_block_size / 512);
        int ret = s_dev->read(s_dev, lba, s_block_size / 512,
                              s_cache[idx].data);
        if (ret < 0)
            return (uint8_t *)0;
    } else {
        s_cache_age++;
        s_cache[idx].age = s_cache_age;
    }

    return s_cache[idx].data;
}

/* ------------------------------------------------------------------ */
/* ext2_sync — flush all dirty cache slots to disk                     */
/* ------------------------------------------------------------------ */

void ext2_sync(void)
{
    int i;
    int flushed = 0;
    if (!s_mounted || !s_dev)
        return;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].dirty && s_cache[i].block_num != 0) {
            uint64_t lba = (uint64_t)s_cache[i].block_num *
                           (s_block_size / 512);
            s_dev->write(s_dev, lba, s_block_size / 512,
                         s_cache[i].data);
            s_cache[i].dirty = 0;
            flushed++;
        }
    }
    printk("[EXT2] sync: flushed %u dirty blocks\n", (uint32_t)flushed);
}
