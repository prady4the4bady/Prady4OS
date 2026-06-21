; user/exectest.asm — execve target image (Phase 5b slice 7, ADR-022).
; ============================================================================
; Static ELF64 linked at 0x8000000000 (like hello/systest). The kernel places
; this image on the FAT32 root as /EXECTEST.ELF; the ring-3 systest program then
; SYS_EXECVE's it. On a successful execve THIS image runs in the SAME process
; (same PID, same kernel stack), proving the image was replaced. It prints one
; sentinel via SYS_WRITE(stdout) — which needs no capability, the inherited
; stdio fd suffices — then SYS_EXIT(0).
; ============================================================================

BITS 64

SYS_WRITE equ 6
SYS_EXIT  equ 4
STDOUT    equ 1

section .text
global _start
_start:
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel msg]
    mov     rdx, msglen
    syscall

    mov     rax, SYS_EXIT
    xor     rdi, rdi                ; exit code 0
    syscall
.hang:
    jmp     .hang                   ; sys_exit does not return; guard anyway

section .rodata
msg:    db "EXECVE: new image running", 10
msglen: equ $ - msg
