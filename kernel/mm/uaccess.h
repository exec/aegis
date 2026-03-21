#ifndef UACCESS_H
#define UACCESS_H

#include "arch.h"
#include <stdint.h>

/* copy_from_user — copy len bytes from user-space src to kernel-space dst.
 *
 * Caller MUST validate [src, src+len) with user_ptr_valid() before calling.
 * Uses a single arch_stac/arch_clac window around the entire copy — one pair
 * of AC-bit transitions regardless of len, replacing the per-byte pattern.
 *
 * The "memory" clobbers inside arch_stac/arch_clac prevent the compiler from
 * hoisting the memcpy before stac or sinking it past clac.
 *
 * No fault recovery: GCC may vectorize __builtin_memcpy, emitting multi-byte
 * loads. If [src, src+len) crosses a page boundary where the second page is
 * unmapped, a #PF fires with AC=1. There is no fixup table (Linux extable).
 * Caller must ensure the entire range is mapped before calling. */
static inline void
copy_from_user(void *dst, const void *src, uint64_t len)
{
    arch_stac();
    __builtin_memcpy(dst, src, len);
    arch_clac();
}

/* copy_to_user — copy len bytes from kernel-space src to user-space dst.
 *
 * Caller MUST validate [dst, dst+len) with user_ptr_valid() before calling.
 * Single arch_stac/arch_clac window around the entire copy.
 * Same fault-recovery caveats as copy_from_user: no extable, caller must
 * ensure the range is mapped. */
static inline void
copy_to_user(void *dst, const void *src, uint64_t len)
{
    arch_stac();
    __builtin_memcpy(dst, src, len);
    arch_clac();
}

#endif /* UACCESS_H */
