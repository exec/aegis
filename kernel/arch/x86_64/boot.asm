; boot.asm — Multiboot2 header, 32-to-64 long mode setup, kernel entry
;
; Entry state per multiboot2 spec:
;   CPU: 32-bit protected mode, interrupts disabled, paging off
;   EAX: multiboot2 magic value (0x36D76289)
;   EBX: physical address of multiboot2 info structure
;   Stack: undefined — we set it up before any C call
;
; This file is x86-specific and intentionally lives in kernel/arch/x86_64/.

MULTIBOOT2_MAGIC  equ 0xE85250D6   ; identifies this as a multiboot2 header
MULTIBOOT2_ARCH   equ 0             ; 0 = i386 (32-bit protected mode entry)
KERNEL_STACK_SIZE equ 0x4000        ; 16KB boot stack

; ─── Multiboot2 header ────────────────────────────────────────────────────────
; Must land within the first 8KB of the ELF image so QEMU -kernel finds it.
; Linker script forces .multiboot to be the very first section.
section .multiboot
align 8
multiboot_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd (multiboot_header_end - multiboot_header_start)
    ; Checksum: magic + arch + length + checksum must sum to 0 (mod 2^32)
    dd -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + \
         (multiboot_header_end - multiboot_header_start))
    ; End tag (type=0, flags=0, size=8)
    dw 0
    dw 0
    dd 8
multiboot_header_end:


; ─── GDT for 64-bit mode ──────────────────────────────────────────────────────
; Loaded in 32-bit mode before the long mode transition.
; Descriptor format: see Intel SDM Vol 3A, Section 3.4.5
section .data
align 8
gdt64:
    ; Entry 0: null descriptor (required)
    dq 0x0000000000000000
    ; Entry 1 (selector 0x08): 64-bit code segment
    ;   Limit: 0xFFFFF (ignored in 64-bit mode), Base: 0
    ;   Access: P=1, DPL=0, S=1, Type=1010 (code/execute/read)
    ;   Flags: G=1, L=1 (64-bit), D=0
    dq 0x00AF9A000000FFFF
    ; Entry 2 (selector 0x10): 64-bit data segment
    ;   Access: P=1, DPL=0, S=1, Type=0010 (data/read/write)
    ;   Flags: G=1, L=0 (data segments ignore L bit)
    dq 0x00AF92000000FFFF
gdt64_end:

; LGDT descriptor: 2-byte limit + 4-byte base (loaded in 32-bit mode)
gdt64_ptr:
    dw (gdt64_end - gdt64 - 1)
    dd gdt64


; ─── Entry point ──────────────────────────────────────────────────────────────
section .text
bits 32

; _start — first instruction executed after the bootloader hands off control.
;
; Purpose: transition from 32-bit protected mode to 64-bit long mode,
;          set up a stack, then call kernel_main.
; Clobbers: all registers
; Calling convention: none (we are the entry point)
global _start
_start:
    cli                         ; disable interrupts (undefined state from bootloader)

    ; Preserve multiboot2 args before we clobber EAX/EBX.
    ; mov edi,eax zero-extends to RDI (first System V AMD64 arg after far jump).
    ; mov esi,ebx zero-extends to RSI (second arg). Both values are < 4GB.
    mov edi, eax                ; mb_magic  → will be first  arg (RDI)
    mov esi, ebx                ; mb_info   → will be second arg (RSI)

    ; ── Load 64-bit GDT (still in 32-bit mode) ──────────────────────────
    lgdt [gdt64_ptr]

    ; ── Enable Physical Address Extension (required for long mode) ───────
    ; CR4.PAE = bit 5
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; ── Build minimal identity-mapping page table ────────────────────────
    ; Layout: PML4 → PDPT → PD → 2MB huge page at physical address 0.
    ; This maps the first 2MB (which contains our kernel at 0x100000)
    ; so we can keep executing after enabling paging.
    ; All three tables are in .bss (zeroed by bootloader); we only write
    ; the entries we need and leave the rest as 0 (= not present).

    ; PML4[0] → PDPT  (present=1, writable=1 → flags 0x3)
    mov eax, pdpt_table
    or  eax, 0x3
    mov [pml4_table], eax

    ; PDPT[0] → PD  (present=1, writable=1)
    mov eax, pd_table
    or  eax, 0x3
    mov [pdpt_table], eax

    ; PD[0] → 2MB huge page at physical 0  (present=1, writable=1, PS=1 → 0x83)
    mov dword [pd_table], 0x83

    ; Load PML4 as the page-table root
    mov eax, pml4_table
    mov cr3, eax

    ; ── Set EFER.LME (Long Mode Enable) via MSR 0xC0000080 ───────────────
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)           ; EFER.LME
    wrmsr

    ; ── Enable paging (activates compatibility mode) ─────────────────────
    ; Also set CR0.WP (write protect) so ring-0 respects read-only pages.
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16)   ; CR0.PG | CR0.WP
    mov cr0, eax

    ; CPU is now in IA-32e compatibility submode.
    ; A far jump to a 64-bit code segment selector activates true 64-bit mode.

    ; ── Far jump: reload CS with 64-bit code descriptor ──────────────────
    ; Selector 0x08 = GDT entry 1 (64-bit code), RPL=0
    jmp 0x08:.long_mode_entry


bits 64
; .long_mode_entry — executes in true 64-bit long mode
;
; Purpose: set up segment registers and stack, then call kernel_main.
; Clobbers: AX, RSP (and anything kernel_main touches)
.long_mode_entry:
    ; ── Load 64-bit data segment into DS, ES, SS ─────────────────────────
    ; Selector 0x10 = GDT entry 2 (64-bit data). FS and GS unused in Phase 1.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; ── Set up the boot stack ─────────────────────────────────────────────
    ; boot_stack_top is the high end of a 16KB zero-initialised buffer in .bss.
    ; 16-byte aligned as required by the System V AMD64 ABI.
    mov rsp, boot_stack_top

    ; ── Call kernel_main(uint32_t mb_magic, void *mb_info) ───────────────
    ; RDI = mb_magic (set at _start, zero-extended from EAX)
    ; RSI = mb_info  (set at _start, zero-extended from EBX; physical addr < 4GB)
    ; Both are already in the correct registers from the moves at _start.
    extern kernel_main
    call kernel_main

    ; ── Hang if kernel_main returns (it must not) ─────────────────────────
.hang:
    hlt
    jmp .hang


; ─── BSS: page tables and boot stack ─────────────────────────────────────────
; The bootloader zeros .bss before calling _start.
; Page tables must be 4KB-aligned and zeroed (unused entries = not present).
section .bss
align 4096
pml4_table: resb 4096
align 4096
pdpt_table: resb 4096
align 4096
pd_table:   resb 4096

align 16
boot_stack:     resb KERNEL_STACK_SIZE
boot_stack_top:
