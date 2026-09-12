; arch/x86_64/boot.asm
; ============================================================================
; NEXUS kernel — higher-half 64-bit entry stub (Phase 2b VMM).
;
; The bootloader maps the kernel at the higher-half virtual base
; 0xFFFFFFFF80000000 (-> physical 0x400000, DDR-733) plus a low identity map, then jumps
; here at the high virtual address with RDI = boot_info pointer (SysV arg 1).
;
; This stub: switches to the kernel's own .bss stack (high virtual), zeroes
; .bss (NOLOAD, so not loaded from disk), then calls kmain(boot_info*). RDI must
; be preserved across the bss-zeroing (it holds the only pointer to boot_info).
; ============================================================================

BITS 64

KSTACK_SIZE equ 16384

section .text.boot
global kernel_entry
; DDR-1106: the BSP idle thread (idle0) runs on this stack and never leaves it,
; so init_idle can record its base in the tcb and DDR-1105's next->rsp check
; stops skipping idle0. Exported for that one reader; nothing else takes it.
global kernel_stack
extern kmain
extern __bss_start
extern __bss_end

kernel_entry:
    lea rsp, [rel kernel_stack_top]   ; high-virtual kernel stack
    mov r15, rdi                      ; preserve boot_info pointer

    ; zero .bss  [__bss_start, __bss_end)  (no stack use; rep does not push)
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    xor eax, eax
    cld
    rep stosb

    xor rbp, rbp                      ; terminate backtrace chain
    mov rdi, r15                      ; restore boot_info -> kmain arg 1
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
; KSTACK_SIZE is 16384 and MUST stay equal to sched.c's STACK_SIZE: DDR-1106
; hands `kernel_stack` to init_idle as idle0's kstack_base, and the window check
; in schedule_locked derives the top as base + STACK_SIZE with no per-thread
; size field. A _Static_assert cannot see a NASM equ, so the pairing is stated
; at both ends and checked by reading (DDR-1106 sec.2).
kernel_stack:
    resb KSTACK_SIZE
kernel_stack_top:
