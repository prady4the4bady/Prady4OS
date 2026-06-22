; arch/x86_64/usermode.asm
; ============================================================================
; NEXUS — ring-3 entry helper.
;
; enter_user_mode builds an IRETQ frame and drops to ring 3 (the standard way to
; *enter* user mode the first time; SYSRET is only for *returning* from a call).
; The ring-3 test program is now a real static ELF (user/hello.asm) loaded from
; SFS by the Phase 5a ELF loader, not an embedded blob.
; ============================================================================

BITS 64

USER_CS  equ 0x20 | 3          ; user code64, RPL 3
USER_SS  equ 0x18 | 3          ; user data,   RPL 3

section .text
global enter_user_mode
global signal_sigreturn

; void enter_user_mode(uint64_t rip /*RDI*/, uint64_t rsp /*RSI*/, uint64_t arg /*RDX*/)
enter_user_mode:
    push qword USER_SS         ; SS
    push rsi                   ; RSP
    push qword 0x202           ; RFLAGS (IF set)
    push qword USER_CS         ; CS
    push rdi                   ; RIP
    mov rdi, rdx               ; user RDI = arg (e.g. the console capability)
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    iretq                      ; -> ring 3

; void signal_sigreturn(struct regs *saved /*RDI*/)
; Restore a full register snapshot taken at signal delivery and IRETQ back to
; ring 3, resuming exactly where the signal interrupted. Offsets match
; kernel/include/regs.h. Never returns.
signal_sigreturn:
    mov rax, rdi               ; rax = &saved
    push qword [rax+168]       ; ss
    push qword [rax+160]       ; rsp
    push qword [rax+152]       ; rflags
    push qword [rax+144]       ; cs
    push qword [rax+136]       ; rip
    mov r15, [rax+0]
    mov r14, [rax+8]
    mov r13, [rax+16]
    mov r12, [rax+24]
    mov r11, [rax+32]
    mov r10, [rax+40]
    mov r9,  [rax+48]
    mov r8,  [rax+56]
    mov rbp, [rax+64]
    mov rdi, [rax+72]
    mov rsi, [rax+80]
    mov rdx, [rax+88]
    mov rcx, [rax+96]
    mov rbx, [rax+104]
    mov rax, [rax+112]         ; rax last (clobbers base pointer)
    iretq                      ; -> ring 3 at the interrupted RIP/RSP
