; user/hello.asm — freestanding ring-3 test program (Phase 5a, ADR-021).
; ============================================================================
; Static ELF64, linked at 0x8000000000 (user range, PML4 slot 1). Prints
; "HELLO FROM RING-3\n" one character at a time via SYS_PUTC, then SYS_EXIT(0).
;
; The kernel delivers the console capability in RDI at entry (5a convention;
; POSIX write() and a proper libc arrive in 5b/5c). RBX/R12/R13 survive each
; SYSCALL because the C dispatch preserves callee-saved registers per the SysV
; ABI — only RAX/RCX/R11 change.
; ============================================================================

BITS 64

SYS_PUTC equ 1
SYS_EXIT equ 4

section .text
global _start
_start:
    mov     rbx, rdi                ; RBX = console capability (preserved)
    lea     r12, [rel msg]          ; R12 = message cursor
    mov     r13, msglen             ; R13 = bytes remaining
.loop:
    test    r13, r13
    jz      .done
    mov     rax, SYS_PUTC
    mov     rdi, rbx                ; arg1 = capability
    movzx   rsi, byte [r12]         ; arg2 = next character
    syscall
    inc     r12
    dec     r13
    jmp     .loop
.done:
    mov     rax, SYS_EXIT
    xor     rdi, rdi                ; exit code 0
    syscall
.hang:
    jmp     .hang                   ; sys_exit does not return; guard anyway

section .rodata
msg:    db "HELLO FROM RING-3", 10
msglen: equ $ - msg
