; user/fputest.asm — 5d FPU-context-switch regression (ADR-023 §D8).
; ============================================================================
; Two instances run concurrently as separate ring-3 processes. Each loads its
; own (unique) pid into XMM0 ONCE, then spins a tight check-loop re-reading XMM0
; and requiring it to still equal its pid. The two are interleaved by PIT
; preemption (no syscalls in the loop — yielding would stall ~20 ms/visit on the
; idle thread and blow the gate timeout). Without per-thread FXSAVE/FXRSTOR the
; other instance's pid clobbers XMM0 the first time this thread is rescheduled
; after the other ran -> mismatch -> "PRADYOS_FPU_FAIL". With it, XMM0 survives
; every switch and the loop completes (~30M iterations, well over the 10000
; required, and long enough to span many 20 ms quanta) -> "PRADYOS_FPU_OK".
;
; r15 = our pid (reference, callee-saved -> survives any preemption's kernel
; path). XMM0 is the only at-risk state — exactly what this exercises. Uses SSE
; (movq xmm0,<gpr>), enabled by the kernel's cpu_enable_sse().
; ============================================================================

BITS 64

SYS_GETPID equ 2
SYS_EXIT   equ 4
SYS_WRITE  equ 6
ITERS      equ 30000000           ; ~30M tight iters: spans many quanta, < timeout

section .text
global _start
_start:
    mov     rax, SYS_GETPID
    syscall
    mov     r15, rax                ; r15 = our pid (distinct per process)
    movq    xmm0, r15               ; seed XMM0 once; preemption must preserve it
    mov     r14, ITERS
.loop:
    movq    rcx, xmm0               ; read XMM0 back
    cmp     rcx, r15
    jne     .fail                   ; clobbered by the other proc -> no FPU save
    dec     r14
    jnz     .loop

    mov     rax, SYS_WRITE          ; survived every context switch
    mov     rdi, 1
    lea     rsi, [rel okmsg]
    mov     rdx, oklen
    syscall
    jmp     .exit
.fail:
    mov     rax, SYS_WRITE
    mov     rdi, 1
    lea     rsi, [rel failmsg]
    mov     rdx, faillen
    syscall
.exit:
    mov     rax, SYS_EXIT
    xor     rdi, rdi
    syscall
.hang:
    jmp     .hang

section .rodata
okmsg:   db "PRADYOS_FPU_OK", 10
oklen:   equ $ - okmsg
failmsg: db "PRADYOS_FPU_FAIL", 10
faillen: equ $ - failmsg
