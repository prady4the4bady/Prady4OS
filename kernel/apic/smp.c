/* kernel/apic/smp.c — SMP bring-up: INIT-SIPI-SIPI, APs parked (ADR-029).
 *
 * The BSP copies the 16-bit trampoline (arch/x86_64/ap_boot.asm) to physical
 * 0x8000, fills its mailbox (kernel CR3, C entry, per-AP stack, cpu index),
 * and sends the MP-init sequence per Intel SDM vol. 3 §8.4: INIT, ~10 ms wait,
 * then two STARTUP IPIs (vector 0x08 -> 0x8000) ~200 µs apart. APs come up one
 * at a time (shared mailbox), announce under a spinlock, mark themselves
 * online, and park (cli/hlt) — no scheduler, no IRQs (ADR-016 stays valid).
 */
#include "smp.h"
#include "lapic.h"
#include "percpu.h"
#include "spinlock.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "irq.h"
#include "io.h"
#include "console.h"
#include "tss.h"
#include "sched.h"
#include "syscall.h"
#include "cpu_mitigations.h"

extern void gdt_init(void);   /* arch/x86_64/cpu.asm — load the shared gdt64 */

#define TRAMP_PHYS    0x8000ull
#define OFF_MB_CR3    0xA0
#define OFF_MB_ENTRY  0xA8
#define OFF_MB_STACK  0xB0
#define OFF_MB_IDX    0xB8
#define OFF_MB_EFER   0xBC          /* DDR-757: EFER OR-mask (LME | NXE?) */

#define IPI_INIT      0x00004500u   /* INIT, assert, fixed dest              */
#define IPI_SIPI      0x00004608u   /* STARTUP, vector 0x08 (page 0x8000)    */

extern const uint8_t ap_tramp_start[], ap_tramp_end[];

static volatile uint32_t g_online = 1;      /* the BSP */

/* DDR-1040: how many CPUs are running. The fault latch refuses to arm unless
 * this is 1 — a latch armed while an AP is live could swallow that AP's fault
 * instead of the probe's, and a swallowed fault resumes at an address that
 * means nothing to the CPU that took it. */
unsigned smp_online(void) { return (unsigned)g_online; }
/* DDR-963 §5: the private announce lock is gone; these sites now take the
 * shared console line lock from console.h, so an [smp] announce can no longer
 * be spliced by a printer that ACQUIRES that lock (the §3 partial case).
 *
 * DDR-970: not by every printer. The ring-3 trap printer uses
 * console_line_trylock() and deliberately prints anyway when the lock is held
 * (a blocking acquire there would turn a fault into a hang), so an exception
 * line can still splice an [smp] announce. The guarantee covers blocking
 * callers only. */

/* ~N ticks of PIT/APIC time (10 ms each); IF is on, so g_ticks advances. */
static void delay_ticks(uint64_t n) {
    uint64_t until = g_ticks + n;
    while (g_ticks < until)
        __asm__ volatile("pause");
}

/* ~200 µs: port-0x80 writes are the classic ~1 µs I/O delay. */
static void delay_200us(void) {
    for (int i = 0; i < 200; i++)
        io_wait();
}

/* C entry for a freshly long-moded AP (jumped to from the trampoline with its
 * cpu index in RDI and a private stack). Announce, mark online, park. */

/* DDR-1040: SMEP. CR4 bit 20 makes a ring-0 INSTRUCTION FETCH through a page
 * whose translation has U/S = user fault — the mitigation for the oldest
 * kernel-exploit primitive there is: corrupt a function pointer, aim it at a
 * page the attacker already owns in their own address space, and the kernel
 * executes it with full privilege. Nothing else here prevents that;
 * vmm_protect_kernel (DDR-757) audits the kernel's OWN PTEs and says nothing
 * about user ones.
 *
 * Safe by construction in this tree (DDR-1040 §3): stage2.asm builds the low
 * identity map with 0x83 and the higher half with 0x3 — U=0 at every level —
 * and VMM_USER_MIN >> 39 == 1, so every user mapping lives in PML4 slot 1,
 * disjoint from the identity map (slot 0) and the kernel (slot 511). No address
 * the kernel executes from can have U=1.
 *
 * CR4 is PER-CPU, so this runs on the BSP and again on every AP. Guarded on
 * CPUID: writing a reserved CR4 bit #GPs, and the default QEMU model does not
 * have SMEP. */
unsigned cpu_enable_smep(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(7), "c"(0));
    unsigned have = (b & (1u << 7)) != 0;      /* CPUID.(7,0):EBX.SMEP */
    if (!have)
        return 0;                              /* bit 0 clear, bit 1 clear */

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 20);                       /* CR4.SMEP */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    /* Read it BACK. "I wrote the bit" and "the bit is set" are different
     * claims, and the gate asserts the second one. */
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return 1u | ((cr4 & (1ull << 20)) ? 2u : 0u);
}

/* DDR-1041: SMAP. CR4 bit 21 makes a ring-0 DATA access through a user
 * translation fault unless EFLAGS.AC is set. uaccess.h's header comment has
 * always CLAIMED that copyin/copyout/copyinstr are the only places the kernel
 * dereferences a user pointer; SMAP is what turns that claim into something the
 * hardware checks, so a violation is a fault that names its own RIP rather than
 * a defect someone has to catch in review.
 *
 * The flag is read by uaccess_begin/uaccess_end, which must NOT emit stac/clac
 * on a CPU without SMAP — those instructions are #UD there, and the TCG default
 * does not have SMAP (DDR-1040 §2). It is written before any AP runs and only
 * ever set, so no ordering beyond a plain store is owed. */
volatile unsigned g_smap_on;            /* BSS: zero until enabled */

unsigned cpu_enable_smap(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(7), "c"(0));
    unsigned have = (b & (1u << 20)) != 0;     /* CPUID.(7,0):EBX.SMAP */
    if (!have)
        return 0;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 21);                       /* CR4.SMAP */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    /* Publish the flag only once the bit is actually set: uaccess_begin reads
     * it to decide whether stac is legal, and a flag set ahead of the bit would
     * be a lie in the one direction that faults. */
    unsigned on = (cr4 & (1ull << 21)) ? 1u : 0u;
    if (on) g_smap_on = 1;
    return 1u | (on << 1);
}

/* DDR-1044: MACHINE-CHECK (#MC, vector 18).
 *
 * MEASURED FIRST, and it is the whole reason this exists. With CR4.MCE clear,
 * a machine check does NOT raise #MC -- it takes the machine down. QEMU says so
 * in as many words when one is injected:
 *
 *     "CPU 0: MCE capability is not enabled, raising triple fault"
 *
 * and the serial log simply STOPS mid-boot: no panic, no banner, no registers,
 * nothing. On real hardware that is a box that dies silently on a memory or
 * cache fault with zero diagnostic. idt.c's panic path already knows vector 18
 * by name and already prints a full register dump; CR4.MCE is what makes that
 * path REACHABLE.
 *
 * SDM Vol.3 §15.8 initialisation order, and each step earns its place:
 *   1. CPUID.1:EDX.MCE (bit 7) must be set or CR4.MCE #GPs.
 *   2. CPUID.1:EDX.MCA (bit 14) gates the MCG and MCi bank MSRs -- without it
 *      the banks do not exist and rdmsr on them #GPs, so a CPU with MCE but
 *      no MCA gets the CR4 bit and no bank programming.
 *   3. MCG_CTL, if MCG_CAP.CTL_P, enables the reporting machinery globally.
 *   4. Per bank: CTL all-ones (report everything), then STATUS cleared -- a
 *      stale VAL bit left from firmware would make the first #MC report a fault
 *      that happened before this kernel booted.
 *   5. CR4.MCE last, so nothing can be delivered before the banks are sane.
 *
 * Per-CPU: CR4 and every MCi_* MSR are per-logical-processor, so every AP runs
 * this too (smp_ap_entry, beside cpu_enable_sse/smep/smap). */
#define MSR_IA32_MCG_CAP     0x179u
#define MSR_IA32_MCG_STATUS  0x17Au
#define MSR_IA32_MCG_CTL     0x17Bu
#define MSR_IA32_MC0_CTL     0x400u

/* Published so the #MC panic decode knows how many banks to walk. Written by
 * every CPU with the same value; BSS, so zero until the BSP runs. */
volatile uint32_t g_mce_report;

unsigned cpu_enable_mce(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpu_cpuid(1, 0, &a, &b, &c, &d);
    unsigned have_mce = (d & (1u << 7)) != 0;      /* CPUID.1:EDX.MCE */
    unsigned have_mca = (d & (1u << 14)) != 0;     /* CPUID.1:EDX.MCA */
    if (!have_mce)
        return 0;

    unsigned banks = 0;
    if (have_mca) {
        uint64_t cap = cpu_rdmsr(MSR_IA32_MCG_CAP);
        banks = (unsigned)(cap & 0xFFu);
        if (cap & (1ull << 8))                      /* MCG_CAP.CTL_P */
            cpu_wrmsr(MSR_IA32_MCG_CTL, ~0ull);
        for (unsigned i = 0; i < banks; i++) {
            cpu_wrmsr(MSR_IA32_MC0_CTL + 4u * i, ~0ull);        /* MCi_CTL    */
            cpu_wrmsr(MSR_IA32_MC0_CTL + 4u * i + 1u, 0ull);    /* MCi_STATUS */
        }
    }

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 6);                             /* CR4.MCE */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));  /* read BACK, do not assume */

    unsigned rep = 1u | ((cr4 & (1ull << 6)) ? 2u : 0u) | (banks << 8);
    g_mce_report = rep;
    return rep;
}

void smp_ap_entry(uint32_t idx);
void smp_ap_entry(uint32_t idx) {
    /* DDR-SMP-3c-cap-1 D3: leave the 3-entry trampoline GDT for the shared
     * gdt64 (user segments + per-CPU TSS descriptors) BEFORE setting the GS
     * base — gdt_init's gs selector reload zeroes it, exactly as the BSP does
     * (main.c gdt_init -> percpu_init_early). percpu_init_cpu's wrmsr then
     * installs this CPU's real GS base. */
    gdt_init();
    percpu_init_cpu(idx);          /* GS base: %gs state usable from here on */
    { uint64_t anfl = console_line_lock();
    kputs("[smp] cpu ");
    kputdec(idx);
    kputs(" up id=");
    kputdec(lapic_apic_id_at(idx));
    kputs("\r\n");
    console_line_unlock(anfl); }

    /* ADR-030 stage 1: exercise the freshly locked allocators from this CPU —
     * a PMM page and a slab object, allocated and returned — proving the lock
     * plumbing works off the BSP before this AP parks. */
    uint64_t page = pmm_alloc_page();
    void *obj = kmalloc(64);
    int ok = (page != 0) && (obj != 0);
    if (obj)  kfree(obj);
    if (page) pmm_free_page(page);
    { uint64_t anfl = console_line_lock();
    kputs("[smp] cpu ");
    kputdec(idx);
    kputs(ok ? " locks OK\r\n" : " locks FAIL\r\n");
    console_line_unlock(anfl); }

    /* ADR-030 stage 2 (DDR-SMP-2): round-trip this CPU's identity (init'd above). */
    struct percpu *pc = this_cpu();
    { uint64_t anfl = console_line_lock();
    kputs("[smp] cpu ");
    kputdec(idx);
    kputs(pc && pc->cpu_idx == idx ? " percpu OK\r\n" : " percpu FAIL\r\n");
    console_line_unlock(anfl); }

    /* ADR-031 cap-1: this CPU's own TSS + GDT descriptor + TR. RSP0 is a
     * placeholder (0) — unused until a ring-3 thread runs here (cap-4); it is
     * set per-thread via tss_set_rsp0 before any CPL3 entry. Prove TR loaded. */
    tss_init_cpu(idx, 0);
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    { uint64_t anfl = console_line_lock();
    kputs("[smp] cpu ");
    kputdec(idx);
    kputs(tr == (uint16_t)(0x28 + idx * 0x10) ? " tss OK\r\n" : " tss FAIL\r\n");
    console_line_unlock(anfl); }
    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);

    /* ADR-031 cap-2b: leave the park loop and JOIN THE SCHEDULER. Load this AP's
     * IDT + enable its LAPIC (wake IPI now, timer in cap-3), then enter the
     * scheduler idle loop — which also drains the directed mailbox (smp_run_on),
     * so smoke-smpjob / smoke-crosswake keep working. Never returns. CPL0->CPL0
     * interrupts still use this stack; ring-3 on APs (needing TSS.RSP0) is cap-4. */
    void idt_load_ap(void);        /* kernel/idt.c — APs boot with a stale IDTR */
    idt_load_ap();
    lapic_ap_enable();
    lapic_timer_ap_arm();          /* cap-3: this AP's own 100Hz preemption tick */
    /* cap-4: ring 3 runs here now — arm the PER-CPU machine state the BSP set
     * for itself in kmain (the trampoline only does PAE+LME): SSE (CR0/CR4 —
     * musl uses XMM; #UD without), EFER.NXE (W^X NX pages fault RSVD without),
     * and the SYSCALL MSRs (EFER.SCE + STAR/LSTAR/SFMASK; #UD without). */
    cpu_enable_sse();
    cpu_enable_smep();      /* DDR-1040: CR4 is per-CPU; the BSP set its own in kmain */
    cpu_enable_smap();      /* DDR-1041: likewise per-CPU */
    cpu_enable_mce();       /* DDR-1044: CR4.MCE + the MCA banks, also per-CPU */
    vmm_enable_nxe_ap();
    syscall_init_ap();
    sched_ap_enter();
}

/* Post fn to an AP's mailbox and wake it (BSP-side, single producer). Returns
 * 0, or -1 if the slot is busy / the CPU is absent (DDR-SMP-3c-alpha). */
int smp_run_on(uint32_t cpu_idx, void (*fn)(void)) {
    struct percpu *pc = percpu_get(cpu_idx);
    if (!pc || !pc->present || __atomic_load_n(&pc->job, __ATOMIC_ACQUIRE))
        return -1;
    __atomic_store_n(&pc->job, fn, __ATOMIC_RELEASE);
    lapic_send_ipi(pc->apic_id, LAPIC_WAKE_VECTOR);   /* fixed delivery, edge */
    return 0;
}

/* cap-2b: nudge every online AP to reschedule — breaks its idle-loop hlt so it
 * walks the ready ring and picks up newly-created work. Reuses the wake IPI
 * (vector 49; its ISR just EOIs). */
void smp_resched_all(void) {
    for (unsigned i = 0; i < PERCPU_MAX; i++) {
        struct percpu *pc = percpu_get(i);
        if (pc && pc->present && !pc->is_bsp)
            lapic_send_ipi(pc->apic_id, LAPIC_WAKE_VECTOR);
    }
}

/* rq-3: directed reschedule kick — wake ONE specific (idle) CPU so it steals a
 * freshly-enqueued thread immediately instead of on its next timer tick. */
volatile uint64_t g_resched_ipis;
int smp_resched_one(uint32_t cpu_idx) {
    struct percpu *pc = percpu_get(cpu_idx);
    if (pc && pc->present && !pc->is_bsp) {
        __atomic_add_fetch(&g_resched_ipis, 1, __ATOMIC_RELAXED);
        lapic_send_ipi(pc->apic_id, LAPIC_WAKE_VECTOR);
        return 1;
    }
    /* DDR-1014: the BSP is deliberately never kicked here, and that silence used
     * to be indistinguishable from a delivered IPI at the call site. Report it. */
    return 0;
}

/* 1 when the AP has drained its mailbox (job finished). */
int smp_job_done(uint32_t cpu_idx) {
    struct percpu *pc = percpu_get(cpu_idx);
    return pc && !__atomic_load_n(&pc->job, __ATOMIC_ACQUIRE);
}

void smp_start_aps(void) {
    unsigned total = lapic_cpu_count();
    if (total < 2) {
        kputs("[smp] cpus online=1/1\r\n");
        return;
    }

    /* Stage the trampoline at 0x8000 (low conventional RAM, reserved by
     * construction — the kernel lives at 0x400000 since DDR-733). */
    uint64_t tlen = (uint64_t)((uintptr_t)ap_tramp_end - (uintptr_t)ap_tramp_start);
    volatile uint8_t *tramp = (volatile uint8_t *)(uintptr_t)TRAMP_PHYS;
    for (uint64_t i = 0; i < tlen; i++)
        tramp[i] = ap_tramp_start[i];

    uint32_t bsp = lapic_id();
    for (unsigned i = 0; i < total; i++) {
        uint32_t id = lapic_apic_id_at(i);
        if (id == bsp)
            continue;
        uint64_t stack = pmm_alloc_pages(2);          /* 16 KiB AP stack */
        if (!stack) {
            kputs("[smp] no AP stack\r\n");
            break;
        }
        *(volatile uint64_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_CR3)   = vmm_kernel_cr3();
        *(volatile uint64_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_ENTRY) = (uint64_t)(uintptr_t)smp_ap_entry;
        *(volatile uint64_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_STACK) = stack + 4 * PAGE_SIZE;
        *(volatile uint32_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_IDX)   = i;
        /* DDR-757: LME always; add NXE (bit 11) when the kernel enabled NX, so
         * the AP arms NXE BEFORE paging — the higher-half kernel-data pages now
         * carry NX and would RSVD-#PF if touched with NXE clear. */
        *(volatile uint32_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_EFER)  =
            0x100u | (vmm_nx_enabled() ? 0x800u : 0u);

        uint32_t before = g_online;
        lapic_send_ipi(id, IPI_INIT);
        delay_ticks(2);                               /* >= 10 ms after INIT */
        lapic_send_ipi(id, IPI_SIPI);
        delay_200us();
        if (g_online == before) {                     /* SDM: second SIPI if needed */
            lapic_send_ipi(id, IPI_SIPI);
            delay_200us();
        }
        uint64_t deadline = g_ticks + 100;            /* <= 1 s to come online */
        while (g_online == before && g_ticks < deadline)
            __asm__ volatile("pause");
        if (g_online == before) {
            kputs("[smp] cpu timed out id=");
            kputdec(id);
            kputs("\r\n");
        }
    }

    kputs("[smp] cpus online=");
    kputdec(g_online);
    kputs("/");
    kputdec(total);
    kputs("\r\n");
}
