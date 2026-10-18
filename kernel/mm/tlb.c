/* kernel/mm/tlb.c — IPI-based cross-CPU TLB shootdown (DDR-1177).
 *
 * One global request slot, serialised by g_shoot_lock (DDR-1177 §3.1).
 *
 * Generation protocol. g_req.gen is a seqcount: the initiator stores an ODD
 * value, fills the fields, then release-stores the next EVEN value. A reader
 * that sees an odd gen, or a gen that changed while it read the fields, does
 * nothing this time (it will be IPI'd or will poll again). Without the odd
 * phase a CPU that is NOT the current target, servicing inline while the next
 * initiator rewrites the slot, could read the old gen with the new target mask
 * and ack a request it never flushed for.
 *
 * Deadlock rules (DDR-1177 §3.7). The whole shootdown runs with IF=0 (so the
 * initiator cannot be preempted or migrated while it owns the slot, and no ISR
 * on this CPU can interleave its own ICR_HI/ICR_LO pair with ours). Because IF
 * is 0, every wait loop here — the lock try-loop and the ack spin — services
 * any pending request INLINE (§3.7 bullet 2). The handler body is idempotent by
 * gen, so the IPI arriving later is harmless. Two concurrent initiators
 * therefore cannot deadlock: the loser services the winner's request while
 * trying for the lock. Never call from NMI or #MC context.
 */
#include "tlb.h"
#include "percpu.h"
#include "lapic.h"
#include "sched.h"
#include "spinlock.h"
#include "vmm.h"

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

struct tlb_shoot {
    uint64_t cr3;                /* AS to flush; 0 = kernel half / all ASes */
    uint64_t va, npages;         /* range; TLB_FULL => full flush           */
    volatile uint64_t gen;       /* seqcount: odd while being written       */
    uint64_t target_mask;        /* bit per cpu_idx                         */
    volatile uint64_t ack_mask;  /* bits set by targets with release        */
};

_Static_assert(PERCPU_MAX <= 64, "tlb masks are uint64_t (DDR-1177 §5)");

static struct tlb_shoot g_req;
static spinlock_t g_shoot_lock = SPINLOCK_INIT;
volatile uint64_t g_tlb_shoot_reqs, g_tlb_shoot_ipis;

static inline void flush_all_local(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(v) :: "memory");
}

static inline void invlpg_local(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* The handler body: flush for the published request if not yet seen, and ack
 * if this CPU is a target. Runs from the ISR and from every wait loop. */
static void tlb_service(struct percpu *pc) {
    uint64_t g = __atomic_load_n(&g_req.gen, __ATOMIC_ACQUIRE);
    if ((g & 1u) || g == pc->shoot_seen_gen)
        return;
    uint64_t va = g_req.va, n = g_req.npages, tm = g_req.target_mask;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_req.gen, __ATOMIC_RELAXED) != g)
        return;                                   /* slot rewritten under us */
    if (n == TLB_FULL || n > TLB_SHOOT_INVLPG_MAX) {
        flush_all_local();
    } else {
        for (uint64_t i = 0; i < n; i++)
            invlpg_local(va + i * 0x1000ull);
    }
    pc->shoot_seen_gen = g;
    uint64_t bit = 1ull << pc->cpu_idx;
    if (tm & bit)
        __atomic_or_fetch(&g_req.ack_mask, bit, __ATOMIC_RELEASE);
}

void tlb_probe_last(uint64_t *gen, uint64_t *mask) {
    *gen = __atomic_load_n(&g_req.gen, __ATOMIC_ACQUIRE);
    *mask = g_req.target_mask;
}

void tlb_shoot_isr(void) {
    struct percpu *pc = this_cpu();
    if (pc)
        tlb_service(pc);
}

void tlb_cpu_ready(void) {
    struct percpu *pc = this_cpu();
    if (!pc)
        return;
    pc->shoot_seen_gen = __atomic_load_n(&g_req.gen, __ATOMIC_ACQUIRE) & ~1ull;
    __atomic_store_n(&pc->shoot_ready, (uint8_t)1, __ATOMIC_RELEASE);
}

void tlb_shoot(uint64_t cr3, uint64_t va, uint64_t npages) {
    unsigned smp_online(void);                 /* kernel/apic/smp.c */
    if (smp_online() <= 1)
        return;                                /* nobody else can hold the entry */

    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    struct percpu *self = this_cpu();
    if (!self || !self->shoot_ready) {
        /* Not yet able to take part (early AP boot): nothing user-visible runs
         * here yet, and a non-ready CPU cannot be acked against. */
        __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
        return;
    }

    while (!spin_trylock(&g_shoot_lock)) {     /* §3.7: service while we wait */
        tlb_service(self);
        __asm__ volatile("pause");
    }

    /* §3.3 target selection: every ready CPU but self, filtered to those whose
     * current thread runs `cr3` (a CPU that switches INTO the AS after this
     * racy read executes a CR3 write, which is a full flush: no PCID, no PTE_G),
     * or every ready CPU for a kernel-half change (cr3 == 0). */
    uint64_t want = cr3 & PTE_ADDR_MASK;
    uint64_t kmaster = vmm_kernel_cr3() & PTE_ADDR_MASK;
    uint64_t mask = 0;
    for (uint32_t i = 0; i < PERCPU_MAX; i++) {
        struct percpu *p = percpu_get(i);
        if (!p || p == self || !p->present ||
            !__atomic_load_n(&p->shoot_ready, __ATOMIC_ACQUIRE))
            continue;
        if (want) {
            struct tcb *t = __atomic_load_n(&p->current, __ATOMIC_ACQUIRE);
            if (!t)
                continue;
            uint64_t rc = t->cr3 ? (t->cr3 & PTE_ADDR_MASK) : kmaster;
            if (rc != want)
                continue;
        }
        mask |= 1ull << p->cpu_idx;
    }

    if (mask) {
        uint64_t g = g_req.gen;                /* even: we own the slot */
        __atomic_store_n(&g_req.gen, g + 1, __ATOMIC_RELAXED);   /* odd: writing */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_req.cr3 = want;
        g_req.va = va & ~0xFFFull;
        g_req.npages = npages;
        g_req.target_mask = mask;
        g_req.ack_mask = 0;
        __atomic_store_n(&g_req.gen, g + 2, __ATOMIC_RELEASE);
        self->shoot_seen_gen = g + 2;          /* our own TLB is already flushed */
        __atomic_add_fetch(&g_tlb_shoot_reqs, 1, __ATOMIC_RELAXED);

        /* The ICR write is UC MMIO: fence the PTE and request stores before it. */
        __asm__ volatile("mfence" ::: "memory");
        for (uint32_t i = 0; i < PERCPU_MAX; i++) {
            if (!(mask & (1ull << i)))
                continue;
            struct percpu *p = percpu_get(i);
            lapic_send_ipi(p->apic_id, 0x4000u | TLB_SHOOT_VECTOR);
            __atomic_add_fetch(&g_tlb_shoot_ipis, 1, __ATOMIC_RELAXED);
        }

        /* MUTANT M2 (DDR-1177): no ack wait. */
    }

    spin_unlock(&g_shoot_lock);
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
}
