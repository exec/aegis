; syscall_entry.asm — SYSCALL landing pad and ring-3 entry helper
;
; syscall_entry: called by CPU on SYSCALL instruction.
;   CPU state on entry:
;     RAX = syscall number
;     RCX = return RIP (user), R11 = saved RFLAGS, RSP = user RSP
;     RDI = arg1, RSI = arg2, RDX = arg3
;     IF=0, DF=0 (set by IA32_SFMASK=0x700)
;
; CR3 / PML4 policy on the syscall path (Phase 5):
;   syscall_entry does NOT switch CR3.  The user PML4 remains loaded
;   throughout syscall_dispatch so that sys_write (and any future
;   syscall) can dereference user virtual addresses directly.  This is
;   safe because:
;     (a) The user PML4 shares the full kernel higher-half (PML4[511])
;         so all kernel code and higher-half stacks are accessible.
;         Kernel task stacks use kva-allocated per-process VAs (higher-half
;         pages present in both master and user PML4 via the shared pdpt_hi).
;     (b) When sched_exit() transitions from a user task to a kernel task
;         it calls vmm_switch_to(master_pml4) before ctx_switch, ensuring
;         ctx_switch always runs with the correct PML4.
;     (c) Timer interrupts (sched_tick) arrive via isr_common_stub which
;         switches to the master PML4 before isr_dispatch, so scheduler
;         bookkeeping always runs under the master PML4.
;
; proc_enter_user: bare iretq label used by proc_spawn to enter ring 3
;   for the first time. ctx_switch's ret lands here; RSP points at an
;   iretq frame built by proc_spawn. Must NOT have a C prologue.

bits 64
section .text

extern syscall_dispatch
extern g_kernel_rsp
extern g_user_rsp

global syscall_entry
global proc_enter_user

syscall_entry:
    ; ── Step 1: switch to kernel stack ──────────────────────────────────────
    ; RSP is still user RSP — stash it in g_user_rsp, load kernel stack.
    mov  [rel g_user_rsp], rsp
    mov  rsp, [rel g_kernel_rsp]

    ; ── Step 2: save restore frame on kernel stack ───────────────────────────
    ; Pushed deepest-first so the pop order on return is r11, rcx, rsp.
    push qword [rel g_user_rsp]   ; user RSP  (deepest = [rsp+16] after all pushes)
    push rcx                       ; return RIP
    push r11                       ; RFLAGS    (top, popped first on return)

    ; ── Step 3: save r8/r9/r10; build 8-arg SysV call ────────────────────────
    ; Linux: rax=num, rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4, r8=arg5, r9=arg6
    ; New SysV 8-arg: rdi=frame, rsi=num, rdx=arg1, rcx=arg2, r8=arg3,
    ;                 r9=arg4, [rsp+8]=arg5(r8), [rsp+16]=arg6(r9) after call.
    ;
    ; Push r8/r9/r10 for restore-on-return AND as the syscall_frame_t body
    ; (frame->r10 is at the lowest address = struct base).
    push r8          ; save user r8  (also frame->r8 at +16)
    push r9          ; save user r9  (also frame->r9 at +8)
    push r10         ; save user r10 (also frame->r10 at +0 = struct base)
    ; rsp → frame base (&frame->r10)

    ; Push two stack args for 8-arg call (deepest = arg6 first):
    push r9          ; → [rsp+16] after call = arg6 (user r9)
    push r8          ; → [rsp+8]  after call = arg5 (user r8)
    ; rsp → top of stack arg slots; frame is at [rsp+16]

    ; Shuffle registers (dependency-safe order):
    mov  r9,  r10    ; SysV arg6 = arg4 = user r10
    mov  r8,  rdx    ; SysV arg5 = arg3 = user rdx
    mov  rcx, rsi    ; SysV arg4 = arg2 = user rsi
    mov  rdx, rdi    ; SysV arg3 = arg1 = user rdi
    mov  rsi, rax    ; SysV arg2 = num  = syscall number
    lea  rdi, [rsp+16] ; SysV arg1 = frame ptr (past 2 stack-arg slots)

    call syscall_dispatch
    add  rsp, 16     ; discard two stack-arg slots; rsp → saved r10

    ; Restore user r10/r9/r8
    pop  r10
    pop  r9
    pop  r8
    ; rax = return value from syscall_dispatch

    ; ── Step 4: restore RFLAGS, RIP, RSP; return to ring 3 ──────────────────
    pop  r11          ; RFLAGS
    pop  rcx          ; return RIP
    pop  rsp          ; user RSP  (pop rsp sets RSP=[RSP], not RSP+8)

    o64 sysret       ; sysretq: restore RIP from RCX, RFLAGS from R11, RSP, enter ring 3

; proc_enter_user — switch to user PML4 and iretq into ring 3.
;
; Called via ret from ctx_switch on a new user task's first entry.
; On entry RSP points at the initial kernel stack frame built by proc_spawn:
;
;   [rsp+0]  user PML4 physical address  ← popped into CR3 first
;   [rsp+8]  RIP  (user entry point)
;   [rsp+16] CS   (0x23 = user code | RPL=3)
;   [rsp+24] RFLAGS (0x202 = IF=1 | reserved bit 1)
;   [rsp+32] RSP  (user stack top, 16-byte aligned)
;   [rsp+40] SS   (0x1B = user data | RPL=3)
;
; We are executing on the user task's kva-allocated kernel stack, which is
; mapped in BOTH the master and user PML4 via the shared pdpt_hi PDPT page.
; Switching CR3 here (on the kva stack) is safe: after the CR3 switch the
; stack is still accessible in the user PML4, and iretq consumes the
; remaining 5 qwords from the same stack.
;
; This is the ONLY correct place to switch CR3 to the user PML4 on first
; entry.  sched_tick must NOT switch CR3 before ctx_switch because switching
; mid-context-switch would leave the outgoing task's stack live on the CPU
; under the wrong CR3, causing a triple fault on the very next stack access.
proc_enter_user:
    pop  rax          ; user PML4 physical address
    mov  cr3, rax     ; switch to user PML4 — safe, on KSTACK_VA (shared)
    iretq

; fork_child_return was removed in Phase 15 fix.
; The child now enters user space via isr_post_dispatch (iretq path)
; using a complete fake isr_common_stub frame built by sys_fork.
; See kernel/arch/x86_64/isr.asm:isr_post_dispatch.
