/* kernel/arch/x86_64/cpu_mitigations.c — enable available CPU speculative-
 * execution mitigations (IMP-A). Defense-in-depth: where the CPU advertises the
 * controls (CPUID.7.0:EDX), turn on IBRS/STIBP/SSBD via IA32_SPEC_CTRL and fire
 * an IBPB barrier once. QEMU's default TCG CPU advertises none of these bits, so
 * the function reports all-zero and writes no MSR — the gate just checks the
 * "[cpu] mitigations:" line is printed. */
#include "cpu_mitigations.h"
#include "console.h"

void cpu_mitigations_init(void) {
    uint32_t eax, ebx, ecx, edx;
    int ibrs = 0, stibp = 0, ssbd = 0, ibpb = 0;

    cpu_cpuid(0, 0, &eax, &ebx, &ecx, &edx);        /* EAX = max standard leaf */
    if (eax >= 7) {
        cpu_cpuid(7, 0, &eax, &ebx, &ecx, &edx);    /* structured ext features */
        ibrs  = (int)((edx >> 26) & 1u);            /* IBRS/IBPB (IA32_SPEC_CTRL) */
        stibp = (int)((edx >> 27) & 1u);
        ssbd  = (int)((edx >> 31) & 1u);
        ibpb  = ibrs;                               /* IBPB is gated by the same bit 26 */
    }

    /* IA32_SPEC_CTRL exists iff any of IBRS/STIBP/SSBD is advertised; only then
     * is it safe to write (a blind wrmsr on an unsupporting CPU would #GP). */
    if (ibrs || stibp || ssbd) {
        uint64_t spec = (uint64_t)ibrs
                      | ((uint64_t)stibp << 1)
                      | ((uint64_t)ssbd  << 2);
        cpu_wrmsr(MSR_IA32_SPEC_CTRL, spec);
    }
    if (ibpb)
        cpu_wrmsr(MSR_IA32_PRED_CMD, 1ull);         /* flush indirect predictors */

    kputs("[cpu] mitigations: IBRS=");
    kputdec((uint64_t)ibrs);
    kputs(" STIBP=");
    kputdec((uint64_t)stibp);
    kputs(" SSBD=");
    kputdec((uint64_t)ssbd);
    kputs(" IBPB=");
    kputdec((uint64_t)ibpb);
    kputs("\r\n");
}

/* Enable x87 + SSE so ring-3 C code can run (PROC-D, ADR-023 §D8). The x86_64
 * SysV ABI uses XMM registers (e.g. the variadic prologue of printf), which
 * #UD without CR4.OSFXSR. The kernel itself is built -mgeneral-regs-only and
 * never touches the FPU/XMM. NOTE: this does NOT save/restore FPU+XMM state on
 * context switch, so it is correct only while at most ONE thread uses the FPU at
 * a time (true today: only musl-linked C programs do, and they run one at a
 * time). Concurrent C processes (5d+) require per-thread FXSAVE/FXRSTOR — see
 * ADR-023 §D8 deferral. */
void cpu_enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ull << 2);                 /* CR0.EM = 0: no x87 emulation trap   */
    cr0 |=  (1ull << 1);                 /* CR0.MP = 1: monitor coprocessor     */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);   /* CR4.OSFXSR | CR4.OSXMMEXCPT          */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}
