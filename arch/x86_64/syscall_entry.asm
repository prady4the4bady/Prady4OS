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
extern syscall_kstack_top
extern syscall_user_rsp
extern syscall_user_rip

syscall_entry:
    mov [rel syscall_user_rsp], rsp     ; stash user RSP briefly (single-CPU, IF=0)
    mov [rel syscall_user_rip], rcx     ; stash user return RIP (fork's child resume point)
    mov rsp, [rel syscall_kstack_top]   ; switch to this thread's kernel stack

    push qword [rel syscall_user_rsp]   ; save user RSP on the kernel stack
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
    o64 sysret                          ; return to ring 3
