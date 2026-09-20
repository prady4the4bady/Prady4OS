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
    /* DDR-1126: CR0.WP = 1. Without it the R/W bit in a PTE is ADVISORY FOR
     * CPL 0, so vmm_protect_kernel()'s `e &= ~VMM_RW` over .text and .rodata
     * bought nothing against ring 0 — DDR-1125 measured the audit printing
     * "[wx] kernel W^X OK" and a ring-0 write to a page that loop had just
     * stamped completing on the very next line. The NX half was always real
     * (EFER.NXE + PTE bit 63, independent of WP); this is the other half.
     *
     * Unconditional, unlike NX: WP is architectural on every x86 from the 486
     * onward, with no CPUID feature bit to probe and no MSR to enable, so there
     * is nothing here to gate on (contrast vmm.c:34, which must probe
     * CPUID 8000_0001h EDX[20] before touching EFER.NXE).
     *
     * HERE rather than in vmm_protect_kernel() because CR0 is PER-CPU and this
     * function is the one site both paths already run: the BSP at main.c:4001
     * and every AP at smp.c:276 — and it already does a CR0 read-modify-write,
     * so the cost is zero instructions. It must precede vmm_protect_kernel(),
     * and does (4001 < 4006).
     *
     * It ESTABLISHES the value rather than inheriting it, which matters across
     * the two boot paths: stage2.asm never touches bit 16, so the BIOS path
     * took the architectural reset value (0x60000010, bit 16 clear), while
     * boot/uefi/loader.c contains no CR0 reference at all, so the UEFI path took
     * whatever firmware left — and UEFI does not pin WP. Two arms of one ISO
     * that need not have agreed (§INV.13's class, in the form where the property
     * was implemented in NEITHER path). Setting it here makes that moot. */
    cr0 |=  (1ull << 16);                /* CR0.WP = 1: enforce PTE R/W in ring 0 */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);   /* CR4.OSFXSR | CR4.OSXMMEXCPT          */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}
