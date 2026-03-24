#include "tss.h"
#include "../core/printk.h"

/* Verify the TSS is exactly 104 bytes as per x86-64 spec. */
_Static_assert(sizeof(aegis_tss_t) == 104, "TSS must be 104 bytes");

/* Static TSS — zeroed by GRUB ELF loader (BSS). */
static aegis_tss_t s_tss;

/* 4KB stack for the double-fault (#DF) IST handler.
 * Must be 16-byte aligned. CPU switches to this stack when #DF fires
 * regardless of the current RSP value, preventing triple fault on stack
 * overflow or RSP corruption.  IST1 in TSS (s_tss.ist[0], 0-indexed C
 * array) corresponds to IST field value 1 in the IDT gate descriptor
 * (Intel spec uses 1-based numbering). */
static uint8_t s_df_stack[4096] __attribute__((aligned(16)));

/*
 * g_kernel_rsp — kernel stack RSP for SYSCALL entry stub.
 * Updated by arch_set_kernel_stack() alongside tss.rsp0.
 * Exported (no static) so syscall_entry.asm can reference it.
 *
 * g_user_rsp — scratch global for SYSCALL stack switch.
 * Stores user RSP transiently between "mov [g_user_rsp], rsp"
 * and the push to the kernel stack. Single-core only.
 *
 * g_master_pml4 — physical address of the master (kernel) PML4.
 * Set by arch_set_master_pml4() after vmm_init() and referenced from
 * isr.asm and syscall_entry.asm to restore the master PML4 at the start
 * of every interrupt/syscall.  This ensures all kernel code (ISR handlers,
 * syscall dispatch, scheduler) runs with the master PML4 where TCBs and
 * kernel stacks are accessible via the identity map.
 */
uint64_t g_kernel_rsp  = 0;
uint64_t g_user_rsp    = 0;
uint64_t g_master_pml4 = 0;

aegis_tss_t *
arch_tss_get(void)
{
	return &s_tss;
}

void
arch_tss_init(void)
{
	s_tss.iomap_base = 104;   /* offset past end of TSS → I/O bitmap disabled */
	/* IST1 (s_tss.ist[0] in 0-indexed C array) — used by the #DF gate.
	 * Store the TOP of the stack (stack grows downward). */
	s_tss.ist[0] = (uint64_t)&s_df_stack[sizeof(s_df_stack)];
	printk("[TSS] OK: RSP0 initialized\n");
}

void
arch_set_kernel_stack(uint64_t rsp0)
{
	s_tss.rsp0   = rsp0;
	g_kernel_rsp = rsp0;
}

void
arch_set_master_pml4(uint64_t pml4_phys)
{
	g_master_pml4 = pml4_phys;
}
