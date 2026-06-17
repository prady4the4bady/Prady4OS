; arch/x86_64/usermode.asm
; ============================================================================
; NEXUS — ring-3 entry helper + a position-independent user-mode test program.
;
; enter_user_mode builds an IRETQ frame and drops to ring 3 (the standard way to
; *enter* user mode the first time; SYSRET is only for *returning* from a call).
;
; user_blob is raw, position-independent machine code (only immediates + SYSCALL
; + a rip-relative jump) that the kernel copies into a user page and runs at a
; user virtual address. It exercises the NSI from ring 3.
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

; --- user-mode test program (copied to a user page, run in ring 3) ----------
global user_blob_start
global user_blob_end
user_blob_start:
    mov rbx, rdi               ; RBX = console capability (preserved across syscalls)

    mov rax, 1                 ; SYS_PUTC 'U'
    mov rdi, rbx
    mov rsi, 'U'
    syscall
    mov rax, 1                 ; 'S'
    mov rdi, rbx
    mov rsi, 'R'
    syscall
    mov rax, 1                 ; ':'
    mov rdi, rbx
    mov rsi, ':'
    syscall

    mov rax, 2                 ; SYS_GETPID
    syscall
    add rax, 0x30              ; pid -> ASCII digit
    mov rsi, rax
    mov rax, 1                 ; SYS_PUTC <pid>
    mov rdi, rbx
    syscall

    mov rax, 1                 ; newline
    mov rdi, rbx
    mov rsi, 10
    syscall

    mov rax, 3                 ; SYS_YIELD
    syscall

    mov rax, 4                 ; SYS_EXIT(0)
    xor rdi, rdi
    syscall
.spin:
    jmp .spin                  ; (never reached)
user_blob_end:
