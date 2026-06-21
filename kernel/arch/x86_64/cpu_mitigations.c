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
