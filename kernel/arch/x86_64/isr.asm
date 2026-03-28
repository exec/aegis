; isr.asm — Interrupt Service Routine entry stubs for Aegis
;
; Each stub pushes a uniform stack frame (fake error code for exceptions
; that don't push one), then jumps to isr_common_stub.
;
; isr_common_stub saves all GPRs, switches to the master PML4 (if needed),
; calls isr_dispatch(cpu_state_t*), restores CR3 and registers, then iretq.
;
; CR3 save/restore (Phase 5 — user-process support):
;   When an interrupt fires while the user PML4 is loaded (ring-3 context),
;   all kernel code (isr_dispatch, scheduler, printk) must run with the
;   master PML4 so that kva-mapped objects (TCBs, kernel stacks) are
;   reachable via the shared pd_hi page-table chain.
;
;   We push the current CR3 onto the kernel stack (below the cpu_state_t) and
;   restore it before iretq.  Saving CR3 on the stack (not in a global) is
;   correct across context switches: if sched_tick abandons the current ISR
;   frame on the old task's stack and switches to a new task, the saved CR3
;   stays on the old stack and is restored when we context-switch back to it.
;
; Stack layout after all pushes (low address / RSP to high):
;   [RSP+0]   saved CR3   (pushed last by isr_common_stub)
;   [RSP+8]   r15         ← cpu_state_t begins here (rdi = rsp+8)
;   [RSP+16]  r14
;   [RSP+24]  r13
;   [RSP+32]  r12
;   [RSP+40]  r11
;   [RSP+48]  r10
;   [RSP+56]  r9
;   [RSP+64]  r8
;   [RSP+72]  rbp
;   [RSP+80]  rdi
;   [RSP+88]  rsi
;   [RSP+96]  rdx
;   [RSP+104] rcx
;   [RSP+112] rbx
;   [RSP+120] rax
;   [RSP+128] vector
;   [RSP+136] error_code
;   [RSP+144] rip
;   [RSP+152] cs
;   [RSP+160] rflags
;   [RSP+168] rsp  (user; only present on ring-3→ring-0 transition)
;   [RSP+176] ss   (user; only present on ring-3→ring-0 transition)
;
; Vector → macro mapping (Intel SDM Vol 3A Table 6-1):
; ISR_NOERR: 0,1,2,3,4,5,6,7,9,15,16,18,19,20,28,31
; ISR_ERR:   8,10,11,12,13,14,17,21,29,30
; Reserved (install ISR_NOERR as placeholder): 22,23,24,25,26,27
; IRQ stubs (no error code): 0x20-0x2F

bits 64
section .text

extern isr_dispatch
extern signal_deliver
extern g_master_pml4

%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push qword 0    ; fake error code (uniform frame)
    push qword %1   ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr_%1
isr_%1:
    push qword %1   ; vector number (error code already on stack from CPU)
    jmp isr_common_stub
%endmacro

; CPU exceptions 0-31
ISR_NOERR  0   ; #DE divide error
ISR_NOERR  1   ; #DB debug
ISR_NOERR  2   ; NMI
ISR_NOERR  3   ; #BP breakpoint
ISR_NOERR  4   ; #OF overflow
ISR_NOERR  5   ; #BR bound range
ISR_NOERR  6   ; #UD invalid opcode
ISR_NOERR  7   ; #NM device not available
ISR_ERR    8   ; #DF double fault
ISR_NOERR  9   ; coprocessor segment overrun (reserved)
ISR_ERR   10   ; #TS invalid TSS
ISR_ERR   11   ; #NP segment not present
ISR_ERR   12   ; #SS stack fault
ISR_ERR   13   ; #GP general protection
ISR_ERR   14   ; #PF page fault
ISR_NOERR 15   ; reserved
ISR_NOERR 16   ; #MF x87 FP exception
ISR_ERR   17   ; #AC alignment check
ISR_NOERR 18   ; #MC machine check
ISR_NOERR 19   ; #XM SIMD FP exception
ISR_NOERR 20   ; #VE virtualization exception
ISR_ERR   21   ; #CP control protection (#CP has error code)
ISR_NOERR 22   ; reserved
ISR_NOERR 23   ; reserved
ISR_NOERR 24   ; reserved
ISR_NOERR 25   ; reserved
ISR_NOERR 26   ; reserved
ISR_NOERR 27   ; reserved
ISR_NOERR 28   ; #HV hypervisor injection
ISR_ERR   29   ; #VC VMM communication
ISR_ERR   30   ; #SX security exception
ISR_NOERR 31   ; reserved

; LAPIC spurious interrupt vector 0xFF — must NOT send EOI per Intel spec.
; isr_dispatch returns immediately for vector 0xFF.
ISR_NOERR 0xFF

; Hardware IRQs 0x20-0x2F (remapped by PIC)
ISR_NOERR 0x20 ; IRQ0 — PIT timer
ISR_NOERR 0x21 ; IRQ1 — PS/2 keyboard
ISR_NOERR 0x22 ; IRQ2 — cascade (internal)
ISR_NOERR 0x23 ; IRQ3 — COM2
ISR_NOERR 0x24 ; IRQ4 — COM1
ISR_NOERR 0x25 ; IRQ5
ISR_NOERR 0x26 ; IRQ6 — floppy
ISR_NOERR 0x27 ; IRQ7 — LPT1 / spurious master
ISR_NOERR 0x28 ; IRQ8 — RTC
ISR_NOERR 0x29 ; IRQ9
ISR_NOERR 0x2A ; IRQ10
ISR_NOERR 0x2B ; IRQ11
ISR_NOERR 0x2C ; IRQ12 — PS/2 mouse
ISR_NOERR 0x2D ; IRQ13 — FPU
ISR_NOERR 0x2E ; IRQ14 — primary ATA
ISR_NOERR 0x2F ; IRQ15 — secondary ATA / spurious slave

; LAPIC timer vector 0x30 — periodic timer interrupt from local APIC.
ISR_NOERR 0x30

; TLB shootdown IPI vector 0xFE — sent by tlb_shootdown() to flush remote TLBs.
ISR_NOERR 0xFE

; isr_common_stub — save GPRs, switch to master PML4, dispatch, restore, iretq.
;
; Calling convention: SystemV AMD64 ABI — rdi = first argument (cpu_state_t *).
; The cpu_state_t pointer is rsp+8 after the saved-CR3 slot is pushed.
isr_common_stub:
    ; SWAPGS if interrupt came from ring 3 (user mode).
    ; At this point: [RSP+0]=vector, [RSP+8]=error_code, [RSP+16]=RIP, [RSP+24]=CS.
    ; CS == 0x23 means user code segment (RPL=3).  Must swap GS.base before
    ; touching any per-CPU data so kernel GS.base points to percpu_t.
    cmp  qword [rsp + 24], 0x23
    jne  .no_swapgs_entry
    swapgs
.no_swapgs_entry:
    ; Save all GPRs (push order: rax first → r15 last at lowest address).
    ; cpu_state_t field order (low→high): r15,r14,...,rax,vector,error,rip,cs,rf,rsp,ss.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save current CR3 on the stack (below cpu_state_t, at [RSP]).
    ; Saving it on the stack rather than a global ensures correctness across
    ; context switches: the saved value stays with this task's stack frame.
    mov  rax, cr3
    push rax                        ; [RSP] = saved CR3

    ; Switch to master PML4 so isr_dispatch and all downstream code can
    ; access kva-mapped kernel objects (TCBs, stacks) via the shared pd_hi.
    ; Skip if g_master_pml4 is 0 (early boot before vmm_init) or if CR3
    ; already equals master PML4 (interrupt fired in kernel context).
    mov  rbx, [rel g_master_pml4]
    test rbx, rbx
    jz   .no_switch
    cmp  rax, rbx
    je   .no_switch
    mov  cr3, rbx
.no_switch:

    ; Pass cpu_state_t* as first argument.  cpu_state_t starts at RSP+8
    ; (one qword above the saved-CR3 slot).
    lea  rdi, [rsp + 8]
    call isr_dispatch

    ; Signal delivery — check pending signals before returning to user space.
    ; Only delivers if cpu_state.cs == 0x23 (ring-3 return).
    ; Called while CR3 = master PML4, IF = 0, cpu_state_t intact at [rsp+8].
    ; Must be BEFORE isr_post_dispatch label so fork-child and sigreturn
    ; jump targets bypass this check (they have pending_signals=0 or handler=0).
    lea  rdi, [rsp + 8]   ; cpu_state_t * (same offset as above: rsp+8 skips saved CR3)
    call signal_deliver    ; may call sched_exit (noreturn) or patch s->rip/rsp

; isr_post_dispatch — entry point for fork child's first scheduling.
;
; sys_fork builds a fake isr_common_stub frame on the child's kernel stack
; and sets the ctx_switch return address to this label.  When ctx_switch
; pops the child's callee-saved registers and rets here, the stack looks
; exactly as if isr_dispatch just returned: [RSP] = saved CR3, followed by
; the full cpu_state_t.  The child enters user space via iretq with rax=0
; (fork returns 0 in child) and all registers from the parent's syscall frame.
global isr_post_dispatch
isr_post_dispatch:

    ; Restore the original CR3 (from the stack slot at [RSP]).
    pop  rax                        ; rax = saved CR3
    test rax, rax
    jz   .no_restore
    mov  cr3, rax
.no_restore:

    ; Restore GPRs in reverse push order.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; SWAPGS if returning to ring 3 (user mode).
    ; After GPR restores: [RSP+0]=vector, [RSP+8]=error_code, [RSP+16]=RIP, [RSP+24]=CS.
    cmp  qword [rsp + 24], 0x23
    jne  .no_swapgs_exit
    swapgs
.no_swapgs_exit:
    add rsp, 16     ; discard vector + error_code
    iretq

; Jump table — isr_stubs[i] = pointer to isr_i
; idt.c references this as: extern void *isr_stubs[48];
section .data
global isr_stubs
isr_stubs:
    dq isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    dq isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    dq isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    dq isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    dq isr_0x20, isr_0x21, isr_0x22, isr_0x23
    dq isr_0x24, isr_0x25, isr_0x26, isr_0x27
    dq isr_0x28, isr_0x29, isr_0x2A, isr_0x2B
    dq isr_0x2C, isr_0x2D, isr_0x2E, isr_0x2F

; LAPIC timer vector stub — idt.c references as: extern void *isr_stub_lapic_timer;
global isr_stub_lapic_timer
isr_stub_lapic_timer:
    dq isr_0x30

; TLB shootdown IPI stub — idt.c references as: extern void *isr_stub_tlb_shootdown;
global isr_stub_tlb_shootdown
isr_stub_tlb_shootdown:
    dq isr_0xFE

; LAPIC spurious vector stub — idt.c references as: extern void *isr_stub_spurious;
global isr_stub_spurious
isr_stub_spurious:
    dq isr_0xFF
