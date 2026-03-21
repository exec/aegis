#ifndef AEGIS_ELF_H
#define AEGIS_ELF_H

#include <stdint.h>
#include <stddef.h>

/* elf_load_result_t — returned by elf_load on success.
 *   entry:       ELF entry point virtual address
 *   brk:         first byte after last loaded segment (page-aligned)
 *   phdr_va:     virtual address of the program header table in loaded image
 *                (= first PT_LOAD's p_vaddr + e_phoff)
 *   phdr_count:  number of program headers (e_phnum) -- for AT_PHNUM auxv entry
 */
typedef struct {
    uint64_t entry;
    uint64_t brk;
    uint64_t phdr_va;
    uint32_t phdr_count;
} elf_load_result_t;

/* elf_load -- parse ELF64; map all PT_LOAD segments into pml4_phys.
 * Returns 0 on success; fills *out. Returns -1 on parse error.
 * phdr_va = first PT_LOAD p_vaddr + e_phoff (VA of program header table).
 * phdr_count = e_phnum. */
int elf_load(uint64_t pml4_phys, const uint8_t *data,
             size_t len, elf_load_result_t *out);

#endif /* AEGIS_ELF_H */
