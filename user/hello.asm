; user/hello.asm — freestanding ring-3 test program (Phase 5a, ADR-021).
; ============================================================================
; Static ELF64, linked at 0x8000000000 (user range, PML4 slot 1). Prints
; "HELLO FROM RING-3" as ONE SYS_WRITE, then the newline via SYS_PUTC, then
; SYS_EXIT(0).
;
; DDR-730 (SMP console semantics): the line goes out in a single SYS_WRITE so
; the kernel emits it as one locked unit (kwrite under g_console_lock) — under
; real SMP this thread runs on an AP concurrently with BSP kputs traffic, and
; the original per-char SYS_PUTC loop interleaved mid-line with other CPUs'
; output (the smoke gates grep for the intact line). The trailing newline still
; goes via SYS_PUTC to keep NSI 1 covered; a lone '\n' interleaves harmlessly.
;
; The kernel delivers the console capability in RDI at entry (5a convention).
; RBX survives each SYSCALL because the C dispatch preserves callee-saved
; registers per the SysV ABI — only RAX/RCX/R11 change.
; ============================================================================

BITS 64

SYS_PUTC  equ 1
SYS_EXIT  equ 4
SYS_WRITE equ 6

section .text
global _start
_start:
    mov     rbx, rdi                ; RBX = console capability (preserved)
    mov     rax, SYS_WRITE
    mov     rdi, 1                  ; fd 1 = stdout (console)
    lea     rsi, [rel msg]          ; buf
    mov     rdx, msglen             ; count — one atomic console unit
    syscall
    mov     rax, SYS_PUTC           ; newline via NSI 1 (kept covered)
    mov     rdi, rbx                ; arg1 = capability
    mov     rsi, 10                 ; arg2 = '\n'
    syscall
    mov     rax, SYS_EXIT
    xor     rdi, rdi                ; exit code 0
    syscall
.hang:
    jmp     .hang                   ; sys_exit does not return; guard anyway

section .rodata
msg:    db "HELLO FROM RING-3"
msglen: equ $ - msg
