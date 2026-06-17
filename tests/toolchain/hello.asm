; tests/toolchain/hello.asm
; NASM unit — proves nasm assembles ELF64 objects that lld can link.

bits 64
section .text
global tc_asm_value

; unsigned long tc_asm_value(void) -> returns 42 in rax (SysV ABI)
tc_asm_value:
    mov rax, 42
    ret
