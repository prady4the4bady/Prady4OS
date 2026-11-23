; arch/x86_64/uaccess_copy.asm — DDR-1181 §3.7: the one user-copy instruction.
;
; copyin/copyout/copyinstr validate a user range and then copy through the user
; VA. Once threads share an address space, a sibling on another CPU can munmap
; or mprotect that range between the check and the copy, and the copy then
; faults in ring 0. Every user copy therefore runs through the single
; `rep movsb` below. isr_dispatch recognises a #PF whose RIP is in
; [uaccess_copy_insn, uaccess_copy_insn_end) and resumes at uaccess_copy_fault,
; which returns -EFAULT. Any other ring-0 fault still panics.
;
; The caller opens and closes the SMAP window (uaccess_begin/end) around the
; call, so the fixup path returns with AC still set and the caller's
; uaccess_end clears it.
;
;   long uaccess_copy(void *dst, const void *src, uint64_t n)  -> 0 | -EFAULT

bits 64
section .text

global uaccess_copy
global uaccess_copy_insn
global uaccess_copy_insn_end
global uaccess_copy_fault

uaccess_copy:
    mov rcx, rdx
uaccess_copy_insn:
    rep movsb                ; restartable: a fault leaves RIP on this insn
uaccess_copy_insn_end:
    xor eax, eax
    ret
uaccess_copy_fault:
    mov rax, -14             ; -EFAULT (kernel/include/errno.h)
    ret
