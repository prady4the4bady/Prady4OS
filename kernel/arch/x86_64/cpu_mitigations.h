/* kernel/arch/x86_64/cpu_mitigations.h — speculative-execution mitigations
 * (IMP-A). Probes CPUID.7.0:EDX for IBRS/STIBP/SSBD/IBPB and, where the CPU
 * advertises them, programs IA32_SPEC_CTRL and issues an IA32_PRED_CMD barrier.
 * On QEMU TCG those feature bits read 0, so every wrmsr is skipped — zero
 * functional and CI impact. The inline asm uses the split-operand wrmsr form and
 * explicit cpuid output constraints (never the "A" / "=A" pair on x86-64). */
#pragma once
#include <stdint.h>

#define MSR_IA32_SPEC_CTRL  0x48u         /* bit0 IBRS, bit1 STIBP, bit2 SSBD */
#define MSR_IA32_PRED_CMD   0x49u         /* bit0 IBPB (write-only command)   */
#define MSR_IA32_FS_BASE    0xC0000100u   /* thread pointer base (PROC-D, ADR-023) */

static inline void cpu_wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr"
        : : "c"(msr), "a"((uint32_t)(val)), "d"((uint32_t)((val) >> 32)) : "memory");
}

static inline void cpu_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

void cpu_mitigations_init(void);

/* Enable x87 + SSE (CR0.EM/MP, CR4.OSFXSR/OSXMMEXCPT) so ring-3 C code can run
 * the x86_64 SysV ABI. No per-thread FPU save yet — see ADR-023 §D8. */
void cpu_enable_sse(void);
