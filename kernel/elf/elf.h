#ifndef AEGIS_ELF_H
#define AEGIS_ELF_H

#include <stdint.h>
#include <stddef.h>

/* elf_load — parse ELF64, map all PT_LOAD segments into pml4_phys.
 * Returns entry RIP on success, 0 on parse error.
 * *out_brk is set to the first page-aligned VA above all loaded segments.
 * Used by proc_spawn to initialise proc->brk. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data,
                  size_t len, uint64_t *out_brk);

#endif /* AEGIS_ELF_H */
