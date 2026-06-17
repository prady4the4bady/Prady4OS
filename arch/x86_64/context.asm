; arch/x86_64/context.asm
; ============================================================================
; NEXUS — kernel thread context switch (Phase 2c).
;
; void context_switch(uint64_t *save_rsp /* RDI */, uint64_t load_rsp /* RSI */)
;
; Saves the SysV callee-saved registers (RBX, RBP, R12-R15) and RFLAGS of the
; current thread onto its stack, stores the resulting RSP into *save_rsp, loads
; the next thread's RSP from load_rsp, and restores its callee-saved state and
; RFLAGS, then RETs into wherever that thread last left off (or its trampoline
; on first run). Caller-saved registers are the C caller's responsibility, so
; they are intentionally not preserved here.
;
; The saved frame, from the saved RSP upward:
;   RFLAGS, R15, R14, R13, R12, RBP, RBX, <return address>
; New threads are seeded with this exact frame by sched_create().
;
; Target: <= 1.5 us cold same-core (see the Layer-2 board); measured at boot.
; ============================================================================

BITS 64

section .text
global context_switch

context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq

    mov [rdi], rsp          ; *save_rsp = current RSP
    mov rsp, rsi            ; switch to the next thread's stack

    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
