; user/argtest.asm — execve argv/envp receiver (DDR-1032).
; ============================================================================
; The TARGET half of the SYS_EXECVE argv/envp test. It is written in assembly
; for one reason: argc/argv/envp are handed over ON THE STACK AT ENTRY, and
; reading them from C means trusting whatever prologue the compiler emitted
; before the first statement. force_align_arg_pointer (which every C probe here
; carries, DDR-823) realigns RSP, so `__builtin_frame_address` cannot be relied
; on to find argc. In assembly the layout is exactly the ABI's:
;
;       [rsp]      argc
;       [rsp+8]    argv[0] .. argv[argc-1]
;       ...        NULL
;       ...        envp[0] .. NULL
;
; It prints one line per string, prefixed so the gate can tell the two vectors
; apart, then the count. A vector delivered in the wrong ORDER, or truncated, is
; therefore visible -- not just "some strings arrived".
; ============================================================================

BITS 64

SYS_WRITE equ 6
SYS_EXIT  equ 4
STDOUT    equ 1

section .text
global _start
_start:
    mov     r12, rsp                ; r12 = &argc, fixed for the whole program
    mov     r13, [r12]              ; r13 = argc

    ; ---- the ABI's alignment requirement, MEASURED at entry ----
    ; SysV enters a process with RSP 16-byte aligned, pointing at argc. The
    ; initial frame is 7 fixed slots plus one per argv/envp string, so it is only
    ; 16-aligned when that total is EVEN -- which is why elf.c pads. Nothing else
    ; in this probe can feel a misaligned stack (assembly, no SSE spills), so a
    ; wrong pad would be silently invisible: this arm exists to make it visible.
    ; It is checked FIRST, before any push, so RSP is exactly as handed over.
    test    rsp, 15
    jnz     .align_bad
    lea     rsi, [rel s_align_ok]
    mov     rdx, s_align_ok_len
    jmp     .align_emit
.align_bad:
    lea     rsi, [rel s_align_bad]
    mov     rdx, s_align_bad_len
.align_emit:
    call    write_str

    ; ---- "PRADYOS_ARGC=" <digit> ----
    lea     rsi, [rel s_argc]
    mov     rdx, s_argc_len
    call    write_str
    mov     rax, r13
    call    write_digit
    call    write_nl

    ; ---- one "PRADYOS_ARGV=" line per argv entry, in order ----
    xor     rbx, rbx                ; i = 0
.argv_loop:
    cmp     rbx, r13
    jge     .argv_done
    lea     rsi, [rel s_argv]
    mov     rdx, s_argv_len
    call    write_str
    mov     rdi, [r12 + 8 + rbx*8]  ; argv[i]
    call    write_cstr
    call    write_nl
    inc     rbx
    jmp     .argv_loop
.argv_done:

    ; ---- envp starts after argv's NULL terminator ----
    lea     r14, [r12 + 8 + r13*8]  ; &argv[argc]  (the NULL)
    add     r14, 8                  ; &envp[0]
.envp_loop:
    mov     rax, [r14]
    test    rax, rax
    jz      .envp_done
    lea     rsi, [rel s_envp]
    mov     rdx, s_envp_len
    call    write_str
    mov     rdi, [r14]              ; RELOAD: write_str clobbers rdi (it puts
                                    ; STDOUT there for the syscall). Loading the
                                    ; pointer before the call is what faulted at
                                    ; cr2=0x1 -- rdi was literally 1. The argv
                                    ; loop above loads after its write_str for
                                    ; the same reason.
    call    write_cstr
    call    write_nl
    add     r14, 8
    jmp     .envp_loop
.envp_done:

    lea     rsi, [rel s_ok]
    mov     rdx, s_ok_len
    call    write_str

    mov     rax, SYS_EXIT
    xor     rdi, rdi
    syscall
.hang:
    jmp     .hang

; write_str: rsi = buf, rdx = len
write_str:
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    syscall
    ret

; write_cstr: rdi = NUL-terminated string
write_cstr:
    mov     rsi, rdi
    xor     rdx, rdx
.len:
    cmp     byte [rsi + rdx], 0
    je      .go
    inc     rdx
    jmp     .len
.go:
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    syscall
    ret

; write_digit: rax = value 0..9
;
; The digit is staged ON THE STACK, not in a .data byte. user.ld links these
; freestanding probes as a SINGLE R+X segment with no writable globals, so a
; `mov [rel dbuf], al` faults: measured, #PF err=0x7 (present, write, user) at
; cr2 inside the image. The stack is the only writable memory a probe of this
; shape has.
write_digit:
    add     al, '0'
    push    rax                     ; the byte now lives at [rsp]
    mov     rsi, rsp
    mov     rdx, 1
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    syscall
    pop     rax
    ret

write_nl:
    lea     rsi, [rel s_nl]
    mov     rdx, 1
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    syscall
    ret

section .rodata
s_argc:     db "PRADYOS_ARGC="
s_argc_len: equ $ - s_argc
s_argv:     db "PRADYOS_ARGV="
s_argv_len: equ $ - s_argv
s_envp:     db "PRADYOS_ENVP="
s_envp_len: equ $ - s_envp
s_ok:       db "PRADYOS_ARGV_OK", 10
s_ok_len:   equ $ - s_ok
s_align_ok:      db "PRADYOS_ARGV_ALIGN=ok", 10
s_align_ok_len:  equ $ - s_align_ok
s_align_bad:     db "PRADYOS_ARGV_ALIGN=bad", 10
s_align_bad_len: equ $ - s_align_bad
s_nl:       db 10
