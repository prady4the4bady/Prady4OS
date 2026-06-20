; user/wxviol.asm — W^X negative regression (Phase 5a, ADR-021).
; ============================================================================
; Attempts to WRITE to its own text page, which the loader mapped read+execute
; (no write bit) per the W^X policy. The store therefore faults with #PF
; (present, write, user) and the kernel converts that ring-3 fault into a clean
; process kill — proving text is genuinely non-writable. If the store were to
; succeed, W^X would be broken; the defensive sys_exit(1) below would run.
; ============================================================================

BITS 64

SYS_EXIT equ 4

section .text
global _start
_start:
    lea     rax, [rel _start]       ; address of our own (RX, non-writable) code
    mov     byte [rax], 0x90        ; WRITE to text -> #PF under W^X
    ; Unreachable if W^X holds. If it does not, fail loudly via a non-zero exit.
    mov     rax, SYS_EXIT
    mov     rdi, 1
    syscall
.hang:
    jmp     .hang
