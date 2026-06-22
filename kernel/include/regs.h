/* kernel/include/regs.h — saved CPU register frame.
 *
 * Layout MUST match the push order in arch/x86_64/isr.asm exactly (isr_dispatch
 * receives a pointer to one of these). Shared by the IDT dispatcher and the
 * signal machinery (which snapshots/restores a frame for sigreturn). The
 * byte offsets are also hard-coded in signal_sigreturn (usermode.asm).
 */
#pragma once
#include <stdint.h>

struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;   /* off 0..56   */
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;       /* off 64..112 */
    uint64_t vector, err_code;                        /* off 120,128 */
    uint64_t rip, cs, rflags, rsp, ss;                /* off 136..168 */
};
_Static_assert(sizeof(struct regs) == 176, "struct regs must be 22*8 bytes");
