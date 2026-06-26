; user/tlstest.asm — PROC-D step 1 ring-3 probe for SYS_SET_TLS + SYS_WRITEV.
; ============================================================================
; Static ELF64 at 0x8000000000 (one R+X segment, W^X by construction). The user
; programs have no writable data segment (user.ld emits text+rodata only), so the
; TLS scratch slot lives on the RW+NX stack the loader sets up.
;
; 1. SYS_SET_TLS: point the FS base at a 64-byte scratch block on the stack,
;    write a magic value through %fs:0, read it back, and require it to match —
;    this proves the kernel programmed IA32_FS_BASE for this thread.
; 2. SYS_WRITEV: gather-write two iovecs to fd 1 (the console). Reached only if
;    the TLS round-trip matched, so the combined banner doubles as the pass mark.
;
; smoke-user greps for "PRADYOS_TLS_OK WRITEV_OK"; a TLS mismatch prints
; "TLS_FAIL" instead and the gate fails.
; ============================================================================

BITS 64

SYS_WRITE   equ 6
SYS_EXIT    equ 4
SYS_SET_TLS equ 27
SYS_WRITEV  equ 28

MAGIC       equ 0x1234DEADBEEF5678

section .text
global _start
_start:
    ; --- (1) thread pointer round-trip -------------------------------------
    sub     rsp, 64                 ; 64-byte TLS scratch block on the stack
    mov     rdi, rsp                ; fs_base = &scratch (in the user range)
    mov     rax, SYS_SET_TLS
    syscall
    test    rax, rax
    jnz     .fail                   ; SET_TLS rejected the base

    mov     rax, MAGIC
    mov     [fs:0], rax             ; store via the new FS base
    mov     rcx, [fs:0]             ; load it back
    mov     rax, MAGIC
    cmp     rcx, rax
    jne     .fail

    ; --- (2) gather-write two iovecs to fd 1 -------------------------------
    sub     rsp, 32                 ; struct iovec iov[2] = {{base,len},{base,len}}
    lea     rax, [rel s1]
    mov     [rsp + 0], rax
    mov     qword [rsp + 8], s1len
    lea     rax, [rel s2]
    mov     [rsp + 16], rax
    mov     qword [rsp + 24], s2len
    mov     rdi, 1                  ; fd = 1 (console)
    mov     rsi, rsp               ; iov
    mov     rdx, 2                ; iovcnt
    mov     rax, SYS_WRITEV
    syscall
    add     rsp, 32
    jmp     .done

.fail:
    mov     rax, SYS_WRITE
    mov     rdi, 1
    lea     rsi, [rel fmsg]
    mov     rdx, fmsglen
    syscall

.done:
    mov     rax, SYS_EXIT
    xor     rdi, rdi
    syscall
.hang:
    jmp     .hang                   ; sys_exit does not return; guard anyway

section .rodata
s1:     db "PRADYOS_TLS_OK "
s1len:  equ $ - s1
s2:     db "WRITEV_OK", 10
s2len:  equ $ - s2
fmsg:   db "TLS_FAIL", 10
fmsglen: equ $ - fmsg
