; user/systest.asm — ring-3 syscall test program (Phase 5b, ADR-022).
; ============================================================================
; A single static ELF (linked at 0x8000000000 like hello/wxviol) that exercises
; the 5b syscalls and prints a sentinel for each outcome. The kernel writes it to
; SFS and loads it back, so it runs in its own W^X address space. This program
; GROWS one slice at a time; each slice appends its tests before sys_exit and the
; matching `smoke-sys*` gate greps the new sentinel lines.
;
; Syscall ABI: number in RAX; args RDI, RSI, RDX, R10; return in RAX.
; ============================================================================

BITS 64

SYS_WRITE equ 6
SYS_EXIT  equ 4

STDOUT    equ 1
EBADF     equ 9          ; returned as -EBADF
EFAULT    equ 14         ; returned as -EFAULT

section .text
global _start
_start:
    ; ---- slice 3: sys_write -------------------------------------------------
    ; Good write to stdout — this line is the headline sentinel.
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_write]
    mov     rdx, m_write_len
    syscall

    ; Bad fd (99) -> -EBADF; on match, print the EBADF sentinel.
    mov     rax, SYS_WRITE
    mov     rdi, 99
    lea     rsi, [rel m_write]
    mov     rdx, m_write_len
    syscall
    cmp     rax, -EBADF
    jne     .no_ebadf
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_ebadf]
    mov     rdx, m_ebadf_len
    syscall
.no_ebadf:

    ; Bad buffer (NULL) -> -EFAULT; on match, print the EFAULT sentinel.
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    xor     rsi, rsi
    mov     rdx, 8
    syscall
    cmp     rax, -EFAULT
    jne     .no_efault
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_efault]
    mov     rdx, m_efault_len
    syscall
.no_efault:

    ; ---- done ---------------------------------------------------------------
    mov     rax, SYS_EXIT
    xor     rdi, rdi
    syscall
.hang:
    jmp     .hang                  ; sys_exit does not return; guard anyway

section .rodata
m_write:     db "SYSWRITE OK", 10
m_write_len: equ $ - m_write
m_ebadf:     db "SYSIO EBADF OK", 10
m_ebadf_len: equ $ - m_ebadf
m_efault:    db "SYSIO EFAULT OK", 10
m_efault_len: equ $ - m_efault
