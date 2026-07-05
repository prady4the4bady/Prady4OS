; arch/x86_64/syscall_entry.asm
; ============================================================================
; NEXUS — SYSCALL entry trampoline (Phase 2e).
;
; Entered by the SYSCALL instruction from ring 3. On entry (set by the CPU):
;   RCX = user return RIP, R11 = user RFLAGS, RSP = user RSP (NOT switched),
;   CS/SS = kernel (from STAR), IF cleared (SFMASK). Syscall ABI:
;   RAX = number; args in RDI, RSI, RDX, R10, R8, R9; return in RAX.
;
; We switch to the current user thread's kernel stack, save the registers the C
; dispatch may clobber (so the user sees them preserved, per the syscall ABI:
; only RAX/RCX/R11 change), marshal up to four args into the SysV C calling
; convention, call syscall_dispatch, then restore and SYSRET back to ring 3.
;
; No nesting: SFMASK clears IF, so a syscall is never interrupted.
; ============================================================================

BITS 64

section .text
global syscall_entry
extern syscall_dispatch

; cap-4 (ADR-031 D5): the user-register snapshot is PER-CPU, stored in the
; percpu area via %gs at fixed offsets (static-asserted in percpu.c). Two CPUs
; in syscalls concurrently no longer clobber each other's fork snapshot.
%define PC_U_RSP    56
%define PC_U_RIP    64
%define PC_U_RBX    72
%define PC_U_RBP    80
%define PC_U_R12    88
%define PC_U_R13    96
%define PC_U_R14    104
%define PC_U_R15    112
%define PC_U_RFLAGS 120

syscall_entry:
    swapgs                              ; DDR-SMP-3a: kernel GS (percpu) active
    mov [gs:PC_U_RSP], rsp              ; stash user RSP (this CPU's slot; IF=0)
    mov [gs:PC_U_RIP], rcx              ; user return RIP (fork's child resume point)
    ; snapshot the user's callee-saved regs + RFLAGS (still the user's values here,
    ; before the C dispatch; fork's child needs them to resume exactly).
    mov [gs:PC_U_RBX], rbx
    mov [gs:PC_U_RBP], rbp
    mov [gs:PC_U_R12], r12
    mov [gs:PC_U_R13], r13
    mov [gs:PC_U_R14], r14
    mov [gs:PC_U_R15], r15
    mov [gs:PC_U_RFLAGS], r11
    mov rsp, [gs:16]                    ; this CPU's kernel stack top (percpu, DDR-SMP-3b)

    push qword [gs:PC_U_RSP]            ; save user RSP on the kernel stack
    push rcx                            ; user RIP
    push r11                            ; user RFLAGS
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    ; marshal: syscall_dispatch(num=RAX, a1=RDI, a2=RSI, a3=RDX, a4=R10)
    mov r8, r10                         ; C arg5 (a4)
    mov rcx, rdx                        ; C arg4 (a3)
    mov rdx, rsi                        ; C arg3 (a2)
    mov rsi, rdi                        ; C arg2 (a1)
    mov rdi, rax                        ; C arg1 (num)
    call syscall_dispatch               ; return value in RAX

    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop r11                             ; user RFLAGS -> R11 (SYSRET restores RFLAGS from R11)
    pop rcx                             ; user RIP   -> RCX (SYSRET jumps to RCX)
    pop rsp                             ; restore user RSP
    swapgs                              ; DDR-SMP-3a: park percpu in KERNEL_GS_BASE
    o64 sysret                          ; return to ring 3
