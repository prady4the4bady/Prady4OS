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
extern __kimg_lo          ; DDR-1179 A2: running base of the image
extern kreloc_table       ; DDR-1179 A1: the encoded relocation table
extern kaslr_slide        ; DDR-1179 A2: published for the printers

kernel_entry:
    lea rsp, [rel kernel_stack_top]   ; high-virtual kernel stack
    mov r15, rdi                      ; preserve boot_info pointer

    ; DDR-1179 A2: self-relocation. slide = running base - link base; the link
    ; base is an immediate, so this instruction carries no relocation itself.
    ; PT_HI maps the image RW here (W^X comes later, vmm_protect_kernel), so
    ; .text fields can be patched. Runs at slide 0 too: every boot decodes the
    ; whole table. Format: "KRL1", u32 count, u32 bytes, then LEB128 varints of
    ; (offset delta << 1 | is64), offsets from the image base.
    lea r8, [rel __kimg_lo]
    mov rax, 0xFFFFFFFF80000000
    mov r9, r8
    mov r9, 0x200000                  ; MUTANT: forced 2 MiB
    lea rsi, [rel kreloc_table]
    cmp dword [rsi], 0x314C524B       ; "KRL1"
    jne .reloc_bad
    mov r10d, [rsi + 4]               ; entry count
    add rsi, 12
    mov rdx, -1                       ; previous offset
.reloc_next:
    test r10d, r10d
    jz .reloc_done
    xor eax, eax
    xor ecx, ecx
.reloc_leb:
    movzx ebx, byte [rsi]
    inc rsi
    mov r11, rbx
    and r11d, 0x7F
    shl r11, cl
    or rax, r11
    add ecx, 7
    test bl, 0x80
    jnz .reloc_leb
    mov r11, rax
    shr r11, 1
    add rdx, r11                      ; this field's offset
    lea rdi, [r8 + rdx]
    test al, 1
    jz .reloc_32s
    add [rdi], r9                     ; R_X86_64_64
    jmp .reloc_adv
.reloc_32s:
    add [rdi], r9d                    ; R_X86_64_32S: the slide is < 1 GiB
.reloc_adv:
    dec r10d
    jmp .reloc_next
.reloc_bad:
    test r9, r9
    jz .reloc_done                    ; slot 0 without a table: nothing moves
.reloc_halt:
    cli                               ; a slid kernel that cannot relocate
    hlt                               ; itself must not run
    jmp .reloc_halt
.reloc_done:
    mov r14, r9                       ; the slide survives the .bss clear

    ; zero .bss  [__bss_start, __bss_end)  (no stack use; rep does not push)
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    xor eax, eax
    cld
    rep stosb

    mov [rel kaslr_slide], r14        ; DDR-1179 A2 (.bss, so after the clear)
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
