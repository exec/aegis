/* kernel/mm/vma.h — per-process Virtual Memory Area tracking */
#ifndef AEGIS_VMA_H
#define AEGIS_VMA_H

#include <stdint.h>

/* VMA type constants */
#define VMA_NONE         0
#define VMA_ELF_TEXT     1   /* PT_LOAD with PROT_EXEC */
#define VMA_ELF_DATA     2   /* PT_LOAD without PROT_EXEC */
#define VMA_HEAP         3   /* [brk_base..brk] */
#define VMA_STACK        4   /* user stack */
#define VMA_MMAP         5   /* anonymous mmap */
#define VMA_THREAD_STACK 6   /* thread stack via pthread_create */
#define VMA_GUARD        7   /* guard page (PROT_NONE) */
#define VMA_SHARED       8   /* MAP_SHARED mapping — phys pages owned by memfd */

typedef struct {
    uint64_t base;
    uint64_t len;
    uint32_t prot;    /* PROT_READ | PROT_WRITE | PROT_EXEC */
    uint8_t  type;    /* VMA_* constant */
    uint8_t  _pad[3];
} vma_entry_t;  /* 24 bytes */

/* Forward-declare to avoid circular include with proc.h */
struct aegis_process;

/* vma_init — allocate a kva page for the VMA table.
 * Sets proc->vma_table, vma_count=0, vma_capacity=170, vma_refcount=1. */
void vma_init(struct aegis_process *proc);

/* vma_insert — add a VMA entry sorted by base address.
 * Merges with adjacent entries if same prot+type. */
void vma_insert(struct aegis_process *proc,
                uint64_t base, uint64_t len, uint32_t prot, uint8_t type);

/* vma_remove — remove [base, base+len) from VMA table.
 * Splits entries at boundaries if partial overlap. */
void vma_remove(struct aegis_process *proc, uint64_t base, uint64_t len);

/* vma_update_prot — change permissions for [base, base+len).
 * Splits entries at boundaries if needed. */
void vma_update_prot(struct aegis_process *proc,
                     uint64_t base, uint64_t len, uint32_t new_prot);

/* vma_clear — set count to 0 (called by execve). */
void vma_clear(struct aegis_process *proc);

/* vma_clone — deep copy VMA table from src to dst (for fork).
 * Allocates a new kva page for dst, copies entries. */
void vma_clone(struct aegis_process *dst, struct aegis_process *src);

/* vma_share — share VMA table from parent to child (for CLONE_VM threads).
 * Increments refcount, copies pointer. */
void vma_share(struct aegis_process *child, struct aegis_process *parent);

/* vma_free — decrement refcount; free kva page if refcount reaches 0. */
void vma_free(struct aegis_process *proc);

#endif /* AEGIS_VMA_H */
