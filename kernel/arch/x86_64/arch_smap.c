#include "arch.h"
#include "printk.h"
#include <stdint.h>

int arch_smap_enabled = 0;

/*
 * arch_sse_init — enable SSE/SSE2 for user-mode code.
 *
 * musl's TLS initialisation uses SSE instructions (movq xmm0, punpcklqdq,
 * movaps) even on a simple binary.  Without this sequence the CPU raises #UD
 * (exception 6) on the first SSE instruction in user space.
 *
 * Required bits:
 *   CR0.EM (bit 2) — must be 0 ("no emulation"; set by BIOS/UEFI, clear it to be safe)
 *   CR0.MP (bit 1) — must be 1 (monitor co-processor, lets WAIT fault on TS)
 *   CR4.OSFXSR     (bit 9)  — OS declares it supports FXSAVE/FXRSTOR
 *   CR4.OSXMMEXCPT (bit 10) — OS handles SSE numeric exceptions (#XF)
 *
 * This must be called before sched_start() hands control to user space.
 * The kernel itself is compiled -mno-sse so the kernel never touches XMM
 * registers; these bits only affect user-mode execution.
 */
void
arch_sse_init(void)
{
    /* Clear CR0.EM (bit 2 = 0x4), set CR0.MP (bit 1 = 0x2) */
    __asm__ volatile (
        "mov %%cr0, %%rax\n"
        "and $0xFFFFFFFFFFFFFFFB, %%rax\n"  /* clear bit 2 (EM) */
        "or  $0x2, %%rax\n"                 /* set   bit 1 (MP) */
        "mov %%rax, %%cr0\n"
        : : : "rax"
    );
    /* Set CR4.OSFXSR (bit 9 = 0x200) and CR4.OSXMMEXCPT (bit 10 = 0x400) */
    __asm__ volatile (
        "mov %%cr4, %%rax\n"
        "or  $0x600, %%rax\n"               /* set bits 9 and 10 */
        "mov %%rax, %%cr4\n"
        : : : "rax"
    );
}

static int
cpuid_smap_supported(void)
{
    uint32_t eax, ebx, ecx, edx;
    /* CPUID leaf 7, subleaf 0, EBX bit 20 = SMAP.
     * cpuid overwrites all four registers; declare them all as outputs so
     * the compiler does not assume EAX/ECX retain their input values.
     * "0"(7) places leaf 7 in EAX (same register as output operand 0);
     * "2"(0) places subleaf 0 in ECX (same register as output operand 2). */
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "0"(7), "2"(0)
    );
    (void)eax; (void)ecx; (void)edx;
    return (ebx >> 20) & 1;
}

void
arch_smap_init(void)
{
    if (!cpuid_smap_supported()) {
        printk("[SMAP] WARN: not supported by CPU\n");
        return;
    }
    /* Set CR4.SMAP (bit 21 = 0x200000) */
    __asm__ volatile (
        "mov %%cr4, %%rax\n"
        "or $0x200000, %%rax\n"
        "mov %%rax, %%cr4\n"
        : : : "rax"
    );
    arch_smap_enabled = 1;
    printk("[SMAP] OK: supervisor access prevention active\n");
}
