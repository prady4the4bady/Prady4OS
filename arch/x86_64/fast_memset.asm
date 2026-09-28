; arch/x86_64/fast_memset.asm
; ============================================================================
; NEXUS — fast memset, sharing DDR-871's ERMS probe (DDR-1076).
;
; void *fast_memset(void *dst /*RDI*/, int c /*ESI*/, size_t n /*RDX*/)
;
; ---------------------------------------------------------------------------
; WHY THIS EXISTS
;
; DDR-871 routed memcpy through REP MOVSB/MOVSQ and left memset as a
; byte-at-a-time C loop. That loop is paid on EVERY kfree (kheap.c:174 poisons
; the object, up to 512 B for the pcb cache, with KHEAP_DEBUG unconditionally
; 1), on every slab growth (kheap.c:326, a full 4096 B), and at four
; init-time page zeroes. DDR-1075 measured the asymmetry; this closes it.
;
; ---------------------------------------------------------------------------
; THE FLAG IS SHARED WITH fast_memcpy, NOT DUPLICATED
;
; fast_memcpy_have_erms is set once by fast_memcpy_init() from CPUID.7:EBX
; bit 9, before any AP starts, and is read-only thereafter. Reading it here
; rather than probing again means the two consumers cannot drift, and there is
; no second init call for a future session to forget to wire.
;
; It lives in .data initialised to 0, so a memset issued BEFORE
; fast_memcpy_init() (main.c:3721) takes the fallback -- which is correct on
; every CPU, so this is a speed property and not a correctness one.
;
; DDR-1076 sec.1 CORRECTS DDR-1075 sec.4.4 ON EXACTLY THAT POINT: `rep stosb`
; is a base x86_64 string instruction. ERMS advertises that the microcode
; implementation is FAST; it does not gate the instruction's existence. A
; build that took the ERMS arm on a pre-ERMS CPU would be correct and slow,
; not wrong -- there is no fault there to hunt for.
;
; ---------------------------------------------------------------------------
; THE BROADCAST IS THE PART THAT CAN GO WRONG
;
; memset fills BYTES with (unsigned char)c, and `rep stosq` moves eight bytes
; per iteration out of RAX -- so c must be replicated into all eight lanes.
; The classic error (movzx and no multiply) leaves c in the low byte and zeros
; above it, which is CORRECT FOR EVERY ZERO FILL and wrong only for a non-zero
; one.
;
; That matters here more than it would elsewhere, because this tree has 72
; memset call sites (73 grep hits, one of them string.c's own definition) and
; exactly THREE with a non-zero fill -- and not one of the three has its bytes
; verified anywhere (DDR-1076 sec.2.1). kheap's
; POISON_FREE is written and never read; the other two are FS write buffers
; checked only by return code. A broadcast defect would therefore be invisible
; to all 177 gates AND to the kheap debug machinery whose poison it corrupts.
; smoke-bench's non-zero-fill arm is what stands in that gap.
;
; AL FALLS OUT FOR FREE: `rep stosq` consumes RAX and `rep stosb` consumes AL,
; and AL is the low byte of the broadcast, i.e. c. So one register serves the
; ERMS path, the fallback bulk AND the fallback byte tail with no reload. The
; obvious alternative -- broadcasting only on the fallback path -- leaves the
; tail's `rep stosb` reading whatever RAX happened to hold.
;
; ---------------------------------------------------------------------------
; COST (item 44 convention -- static figures are hardware-true; a TCG timing
; is valid for regression only and must NOT be quoted as cycles, DDR-870)
;   broadcast:         3 instructions
;   dispatch overhead: 2 instructions + 1 branch
;   ERMS path:         2 instructions after dispatch, 0 stack traffic
;   fallback path:     5 instructions after dispatch, 0 stack traffic
;   no locks, no serialising instructions, no memory allocation
;
; The byte loop this replaces is ~4 instructions PER BYTE (load, store,
; increment, compare-and-branch), so the 4096 B slab-growth zero went from
; ~16,000 retired instructions to ~10 plus one microcoded string operation.
; That is a static count, not a speedup: per DDR-1075 sec.1 a speedup figure
; is not producible under TCG and is not claimed.
;
; DF is clear on entry by the SysV ABI, so no CLD is issued -- the same
; reasoning fast_memcpy.asm records, for the same reason.
; ============================================================================

BITS 64

section .text
global fast_memset
extern fast_memcpy_have_erms

; ---------------------------------------------------------------------------
; fast_memset(dst=RDI, c=ESI, n=RDX) -> dst in RAX
;
; RDI/RCX are string-instruction operands and RAX/R8/R9 are caller-saved in
; SysV, so nothing needs preserving and no stack frame is built.
; ---------------------------------------------------------------------------
fast_memset:
    mov   r8, rdi                   ; hold dst; RDI is consumed by the string op
    movzx rax, sil                  ; RAX = c & 0xFF   (memset fills BYTES)
    mov   r9, 0x0101010101010101
    imul  rax, r9                   ; replicate into all eight lanes

    cmp qword [rel fast_memcpy_have_erms], 0
    je .fallback

    mov rcx, rdx
    rep stosb                       ; consumes AL = c
    mov rax, r8
    ret

.fallback:
    mov rcx, rdx
    shr rcx, 3                      ; whole 8-byte words
    rep stosq                       ; consumes RAX = the broadcast
    mov rcx, rdx
    and rcx, 7                      ; 0-7 byte tail
    rep stosb                       ; AL is still c, the broadcast's low byte
    mov rax, r8
    ret
