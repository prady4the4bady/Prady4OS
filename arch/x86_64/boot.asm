; arch/x86_64/boot.asm
; ============================================================================
; NEXUS kernel — 64-bit entry stub (Phase 2a).
;
; The bootloader (boot/stage2) enters long mode and jumps to the first byte of
; the loaded kernel image, which is this label (placed first via the .text.boot
; section in kernel/kernel.ld). We set a known stack, clear the frame pointer,
; and call into C (kmain). kmain does not return; if it ever does, we halt.
;
; ABI: the bootloader passes a pointer to the boot_info struct (kernel/boot_info.h)
; in RDI per the SysV calling convention. We must NOT clobber RDI before calling
; kmain, which takes it as its first argument: kmain(struct boot_info *).
;
; The bootloader already set RSP, but we set it again so the kernel's entry
; contract does not depend on the loader's choice.
; ============================================================================

BITS 64

KSTACK_TOP equ 0x200000        ; 2 MiB; identity-mapped RAM, away from the image

section .text.boot
global kernel_entry
extern kmain

kernel_entry:
    mov rsp, KSTACK_TOP
    xor rbp, rbp                ; terminate stack-frame chain for backtraces
    call kmain
.hang:
    cli
    hlt
    jmp .hang
