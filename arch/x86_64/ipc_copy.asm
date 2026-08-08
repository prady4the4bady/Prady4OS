; arch/x86_64/ipc_copy.asm
; ============================================================================
; NEXUS — IPC message payload copy (Group 8 item 43, DDR-873).
;
; void ipc_copy32(void *dst /*RDI*/, const void *src /*RSI*/)
;
; The IPC payload is EXACTLY 32 bytes (IPC_MSG_WORDS = 4 × uint64). That single
; fact decides the whole design, so it is stated first: this is not a general
; memcpy and must never be used as one.
;
; ---------------------------------------------------------------------------
; WHY MOVDQU (XMM) AND NOT VMOVDQU (YMM)
;
; 32 bytes is one YMM move or two XMM moves, so YMM looks like the obvious win:
; one instruction instead of four (2 loads + 2 stores). It is not, for two
; reasons that outweigh three saved instructions on a 32-byte copy:
;
;   1. AVX-SSE TRANSITION PENALTY. Touching YMM leaves the upper halves dirty.
;      Every subsequent legacy-SSE instruction — and the kernel is full of them
;      via FXSAVE/FXRSTOR and any compiler-emitted XMM — then pays a
;      save/restore stall on Intel microarchitectures unless VZEROUPPER is
;      issued. VZEROUPPER itself costs more than the instructions saved here.
;
;   2. YMM IS ONLY SAFE WHEN XSAVE IS ON. DDR-872 enables XSAVE with the AVX
;      components where the CPU has them, but on the pre-AVX models the chipset
;      matrix still covers (qemu64, Nehalem) the switch falls back to FXSAVE,
;      which saves XMM but NOT the YMM upper halves. A YMM path would therefore
;      need its own runtime gate, doubling the code for a copy this small.
;
; XMM has neither problem: FXSAVE covers XMM0-15 on every path, so MOVDQU is
; safe with no gate and no transition penalty. SSE2 is architectural on x86_64,
; so there is no CPU without it and no fallback is needed.
;
; This is a case where the queue's suggested instruction is the wrong one, and
; the reason is worth recording rather than silently substituting.
;
; ---------------------------------------------------------------------------
; COST (item 44 convention — static figures are hardware-true)
;   4 instructions: 2 × MOVDQU load, 2 × MOVDQU store
;   32 bytes moved, no stack traffic, no branches, no CPUID gate
;   no AVX state touched, so no VZEROUPPER and no transition penalty
;
; The previous C loop was 4 × (load + store) of 8 bytes plus loop control. This
; halves the memory operations and removes the branch entirely.
; ============================================================================

BITS 64

section .text
global ipc_copy32

ipc_copy32:
    movdqu xmm0, [rsi]              ; bytes 0-15
    movdqu xmm1, [rsi + 16]         ; bytes 16-31
    movdqu [rdi], xmm0
    movdqu [rdi + 16], xmm1
    ret
