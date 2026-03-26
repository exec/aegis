#include "elf.h"
#include "../mm/vmm.h"
#include "kva.h"
#include "../core/printk.h"
#include <stdint.h>
#include <stddef.h>

/* ELF64 header */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

/* ELF64 program header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD    1
#define PF_W       2      /* program header write flag */
#define ELFCLASS64 2
#define ET_EXEC    2
#define EM_X86_64  0x3E

int
elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len,
         elf_load_result_t *out)
{
    (void)len;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Verify ELF magic */
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        printk("[ELF] FAIL: bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_type != ET_EXEC ||
        eh->e_machine != EM_X86_64) {
        printk("[ELF] FAIL: not a static ELF64 x86-64 executable\n");
        return -1;
    }

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(data + eh->e_phoff);
    uint64_t seg_end = 0;
    /* first_pt_load_vaddr: p_vaddr of the first PT_LOAD segment.
     * Used to compute phdr_va = first_pt_load_vaddr + e_phoff, which is
     * the virtual address at which the program header table appears in the
     * loaded image (needed for the AT_PHDR auxv entry). */
    uint64_t first_pt_load_vaddr = 0;
    int found_pt_load = 0;
    uint16_t i;
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;

        if (!found_pt_load) {
            first_pt_load_vaddr = ph->p_vaddr;
            found_pt_load = 1;
        }

        uint64_t this_end = ph->p_vaddr + ph->p_memsz;
        if (this_end > seg_end)
            seg_end = this_end;

        /*
         * Page-align the virtual base downward.
         * ELF segments are not required to start on a page boundary.
         * The mapping must start at the page containing p_vaddr, and
         * the data must be placed at the correct sub-page offset within
         * that first page so the virtual layout is correct.
         *
         * va_base    = page-aligned start of mapping
         * va_offset  = byte offset of p_vaddr within the first page
         * page_count = pages covering [va_base, p_vaddr + p_memsz)
         */
        uint64_t va_base   = ph->p_vaddr & ~4095UL;
        uint64_t va_offset = ph->p_vaddr & 4095UL;

        /* S1: Guard against integer overflow in page_count calculation.
         * A crafted ELF with p_memsz near UINT64_MAX wraps the addition
         * to a small value, causing a tiny allocation but huge copy/zero. */
        if (ph->p_memsz > 0x100000000ULL)   /* 4 GB per-segment cap */
            return -1;
        if (ph->p_filesz > ph->p_memsz)
            return -1;

        /* Allocate kva pages for this segment — no contiguity assumption on
         * physical frames; kva maps each PMM page to a consecutive kernel VA. */
        uint64_t page_count = (va_offset + ph->p_memsz + 4095UL) / 4096UL;
        uint64_t j;

        uint8_t *dst = kva_alloc_pages(page_count);

        /* Zero the first (possibly partial) page so bytes before p_vaddr
         * within it are clean.  Then copy file bytes at the sub-page offset. */
        uint64_t k;
        for (k = 0; k < va_offset; k++)
            dst[k] = 0;

        /* Copy file bytes through kernel VA */
        const uint8_t *src = data + ph->p_offset;
        for (k = 0; k < ph->p_filesz; k++)
            dst[va_offset + k] = src[k];

        /* Zero BSS (bytes past p_filesz up to p_memsz) */
        for (k = ph->p_filesz; k < ph->p_memsz; k++)
            dst[va_offset + k] = 0;

        /* Map each page into the user address space.
         * kva_page_phys recovers the physical address of each page
         * (individual walk — O(page_count × 4) invlpg, acceptable at this scale).
         * Mapping starts at the page-aligned va_base, not raw p_vaddr. */
        uint64_t map_flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W)
            map_flags |= VMM_FLAG_WRITABLE;

        for (j = 0; j < page_count; j++) {
            vmm_map_user_page(pml4_phys,
                              va_base + j * 4096UL,
                              kva_page_phys(dst + j * 4096UL),
                              map_flags);
        }
    }

    if (!found_pt_load) {
        printk("[ELF] FAIL: no PT_LOAD segment\n");
        return -1;
    }

    out->entry       = eh->e_entry;
    out->brk         = (seg_end + 4095UL) & ~4095UL;
    out->phdr_va     = first_pt_load_vaddr + eh->e_phoff;
    out->phdr_count  = eh->e_phnum;
    return 0;
}
