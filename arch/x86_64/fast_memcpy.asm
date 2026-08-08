; arch/x86_64/fast_memcpy.asm
; ============================================================================
; NEXUS — fast memcpy with runtime CPUID dispatch (Group 8 item 42, DDR-871).
;
; void *fast_memcpy(void *dst /*RDI*/, const void *src /*RSI*/, size_t n /*RDX*/)
; void  fast_memcpy_init(void)      — probes CPUID once, at boot
;
; ---------------------------------------------------------------------------
; WHY THERE IS NO AVX2 OR AVX-512 PATH HERE, AND WHY THAT IS NOT AN OMISSION
;
; The queue asks for AVX-512/AVX2/SSE4.2/REP MOVSB. The first two CANNOT be
; used by this kernel today, and writing them anyway would silently corrupt
; ring-3 state:
;
;   * The kernel is built -mgeneral-regs-only and, by design, never touches
;     FPU/XMM/YMM/ZMM (kernel/arch/x86_64/cpu_mitigations.c).
;   * The context switch saves FPU state with FXSAVE — a 512-byte area that
;     holds x87 and XMM0-15 ONLY. It does not save YMM upper halves and does
;     not save ZMM at all; that needs XSAVE with a much larger per-thread area.
;
; So a kernel memcpy touching YMM/ZMM would clobber vector registers belonging
; to whichever ring-3 thread was interrupted, and the corruption would appear
; later, in that thread, as wrong arithmetic — about as far from the cause as a
; bug can get.
;
; Those paths are therefore BLOCKED ON ITEM 18 (AVX-512 state save/restore in
; the context switch). When item 18 lands and the switch uses XSAVE with the
; AVX-512 state components enabled, the dispatch below gains its branches. The
; CPUID probe already records the feature bits so that step is additive.
;
; This is a real dependency the queue lists as two independent items. Recorded
; here rather than discovered later by a corrupted user thread.
;
; ---------------------------------------------------------------------------
; WHAT IS IMPLEMENTED
;
;   ERMS  (CPUID.(EAX=7,ECX=0):EBX bit 9) -> REP MOVSB
;         On every CPU that advertises ERMS, `rep movsb` is the microcoded
;         wide-copy path: it moves a cache line per iteration internally and
;         needs no vector register, so it is both the fastest available option
;         here AND the only wide one that is safe without XSAVE.
;
;   otherwise -> REP MOVSQ for the 8-byte-aligned bulk, then a byte tail.
;         Correct everywhere, including the pre-ERMS machines the chipset
;         matrix still covers (qemu64, Nehalem).
;
; Both paths use only general-purpose registers, so neither disturbs FPU state
; and neither requires a change to the context switch.
;
; COST (item 44 convention — static figures are hardware-true, measured ones
; are QEMU TCG emulated ticks and NOT cycles):
;   dispatch overhead: 2 instructions (load flag, test) + 1 branch
;   ERMS path:         3 instructions after dispatch, 0 stack traffic
;   fallback path:     7 instructions after dispatch, 0 stack traffic
;   no locks, no serialising instructions, no memory allocation
; ============================================================================

BITS 64

section .data
; 1 when the CPU advertises ERMS. Written once by fast_memcpy_init before any
; AP is started, read-only thereafter, so no locking is required.
global fast_memcpy_have_erms
fast_memcpy_have_erms:  dq 0
; Recorded for item 18's benefit; unused until the context switch can save the
; corresponding state. Kept so the probe is not written twice.
global fast_memcpy_have_avx2
fast_memcpy_have_avx2:  dq 0
global fast_memcpy_have_avx512f
fast_memcpy_have_avx512f: dq 0

section .text
global fast_memcpy_init
global fast_memcpy

; ---------------------------------------------------------------------------
; fast_memcpy_init — probe CPUID once.
;
; Guarded on the maximum basic leaf: CPUID leaf 7 does not exist on very old
; CPUs, and executing it there returns whatever leaf the CPU clamps to, which
; would be read as garbage feature bits. Checking leaf 0 first is the standard
; and necessary preamble.
; ---------------------------------------------------------------------------
fast_memcpy_init:
    push rbx                        ; CPUID clobbers RBX; SysV callee-saved
    xor eax, eax
    cpuid                           ; EAX = highest basic leaf
    cmp eax, 7
    jb .done                        ; no leaf 7 -> leave every flag at 0

    mov eax, 7
    xor ecx, ecx
    cpuid                           ; EBX = structured feature flags

    test ebx, 1 << 9                ; ERMS
    jz .no_erms
    mov qword [rel fast_memcpy_have_erms], 1
.no_erms:
    test ebx, 1 << 5                ; AVX2      (recorded, not yet usable)
    jz .no_avx2
    mov qword [rel fast_memcpy_have_avx2], 1
.no_avx2:
    test ebx, 1 << 16               ; AVX-512F  (recorded, not yet usable)
    jz .done
    mov qword [rel fast_memcpy_have_avx512f], 1
.done:
    pop rbx
    ret

; ---------------------------------------------------------------------------
; fast_memcpy(dst=RDI, src=RSI, n=RDX) -> dst in RAX
;
; RDI/RSI/RCX are the string-instruction operands and are all caller-saved in
; SysV, so nothing needs preserving. DF is guaranteed clear on entry by the
; ABI, so no CLD is issued — adding one would cost a pipeline flush on some
; microarchitectures for a condition the ABI already promises.
; ---------------------------------------------------------------------------
fast_memcpy:
    mov rax, rdi                    ; return value = dst

    cmp qword [rel fast_memcpy_have_erms], 0
    je .fallback

    mov rcx, rdx
    rep movsb
    ret

.fallback:
    mov rcx, rdx
    shr rcx, 3                      ; whole 8-byte words
    rep movsq
    mov rcx, rdx
    and rcx, 7                      ; 0-7 byte tail
    rep movsb
    ret
