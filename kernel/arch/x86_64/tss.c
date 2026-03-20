#include "tss.h"
#include "../core/printk.h"

/* Verify the TSS is exactly 104 bytes as per x86-64 spec. */
_Static_assert(sizeof(aegis_tss_t) == 104, "TSS must be 104 bytes");

/* Static TSS — zeroed by GRUB ELF loader (BSS). */
static aegis_tss_t s_tss;

/*
 * g_kernel_rsp — kernel stack RSP for SYSCALL entry stub.
 * Updated by arch_set_kernel_stack() alongside tss.rsp0.
 * Exported (no static) so syscall_entry.asm can reference it.
 *
 * g_user_rsp — scratch global for SYSCALL stack switch.
 * Stores user RSP transiently between "mov [g_user_rsp], rsp"
 * and the push to the kernel stack. Single-core only.
 */
uint64_t g_kernel_rsp = 0;
uint64_t g_user_rsp   = 0;

aegis_tss_t *
arch_tss_get(void)
{
	return &s_tss;
}

void
arch_tss_init(void)
{
	s_tss.iomap_base = 104;   /* offset past end of TSS → I/O bitmap disabled */
	printk("[TSS] OK: RSP0 initialized\n");
}

void
arch_set_kernel_stack(uint64_t rsp0)
{
	s_tss.rsp0   = rsp0;
	g_kernel_rsp = rsp0;
}
