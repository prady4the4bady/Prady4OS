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
;
; ---------------------------------------------------------------------------
; COST (Group 8 item 44, DDR-870)
;
; STATIC — exact, and the only figures here that mean anything on real silicon:
;   17 instructions
;   14 stack accesses (7 push + 7 pop) = 112 bytes of stack traffic
;   2 register moves for the RSP swap, 1 RET
;   no memory allocation, no locks, no CPUID/serialising instruction
;
; The push/pop pairs dominate: every one is a dependent store or load against
; the same stack pointer, so this path is bounded by store-buffer and L1
; latency rather than by instruction count. That is why the register set saved
; here is deliberately only the SysV callee-saved six plus RFLAGS — each extra
; register would add two more dependent memory operations to every switch in
; the system.
;
; MEASURED — QEMU TCG, 2000 iterations, minimum, net of an RDTSC baseline of
; 119 ticks (tools: user/benchtest.c, gate smoke-bench):
;   SYS_YIELD round trip ~170,660 emulated ticks
;
; That figure includes the full syscall entry, schedule(), this switch, and the
; return. It is NOT a hardware cycle count: under TCG a translated instruction
; costs whatever the translation costs, so the number is valid for spotting a
; regression and worthless as an absolute claim. Do not quote it as "cycles".
; ---------------------------------------------------------------------------
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
