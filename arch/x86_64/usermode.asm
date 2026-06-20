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
