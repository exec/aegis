/*
 * mmu_early.c — Page table storage for boot.S to populate.
 *
 * This file used to contain an mmu_early_init() C function that set up
 * TTBR0 identity and TTBR1 kernel mappings. That function is no longer
 * called — boot.S builds the same tables inline in assembly before the
 * MMU is enabled (it has to, since C code can't run at the ELF load
 * address before TTBR1 is up). The storage lives here because .bss
 * page-aligned declarations are cleaner in C than in .S files.
 *
 * Layout boot.S populates (single source of truth — see boot.S's
 * "Build page tables inline" section):
 *
 *   TTBR0 identity map (for the MMU-enable transition):
 *     boot_l0[0] → boot_l1
 *     boot_l1[0] → 0x00000000 (1GB DEVICE)
 *     boot_l1[1] → 0x40000000 (1GB RAM)
 *
 *   TTBR1 kernel map (0xFFFF000000000000+):
 *     kern_l0[0] → kern_l1
 *     kern_l1[0] → 0x00000000   (1GB DEVICE — QEMU GIC/UART + Pi legacy)
 *     kern_l1[1] → 0x40000000   (1GB NORMAL — kernel image region)
 *     kern_l1[2] → 0x80000000   (1GB NORMAL — Pi 4/5 RAM to 3GB)
 *     kern_l1[3] → 0xC0000000   (1GB NORMAL — Pi 4/5 RAM to 4GB)
 *     kern_l1[4] → 0x100000000  (1GB DEVICE — Pi 5 high MMIO: UART
 *                                 0x107D001000, GIC-400 0x107FFF9000)
 *     kern_l1[5] → 0x140000000  (1GB NORMAL — Pi 5 8GB-model RAM)
 *     kern_l1[6] → 0x180000000  (1GB NORMAL)
 *     kern_l1[7] → 0x1C0000000  (1GB NORMAL)
 */

#include <stdint.h>

uint64_t boot_l0[512] __attribute__((aligned(4096)));
uint64_t boot_l1[512] __attribute__((aligned(4096)));
uint64_t kern_l0[512] __attribute__((aligned(4096)));
uint64_t kern_l1[512] __attribute__((aligned(4096)));
