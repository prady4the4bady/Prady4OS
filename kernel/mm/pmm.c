/* kernel/pmm.c — buddy physical frame allocator (ADR-003), Phase 2b.
 *
 * Free blocks are tracked with intrusive singly-linked lists, one per order;
 * the link lives in the free frame itself (valid because managed RAM is
 * identity-mapped). free() coalesces with the XOR-buddy when it is also free.
 * Buddy lookup on free is currently a linear scan of the order's free list —
 * correct and simple; a per-block state bitmap is the obvious later speedup.
 */
#include "pmm.h"
#include "numa.h"      /* DDR-882 17b: per-node free lists */
#include "lapic.h"     /* lapic_id() — which node is this CPU on */
#include "console.h"

#define PMM_MIN_PHYS 0x01000000ull   /* 16 MiB: above kernel/bootloader/BIOS */
#define PMM_MAX_PHYS 0x40000000ull   /* 1 GiB: identity-map limit (ADR-005)   */

#ifdef KASAN
/* Fill every freed frame with a recognizable pattern so a use-after-free read
 * yields obvious garbage instead of stale data, and a later allocation that is
 * read before being initialized is caught. The intrusive free-list link at
 * offset 0 is rewritten by list_push afterwards; the rest of the block stays
 * poisoned. ULL suffix avoids -Woverflow on the 64-bit literal. */
#define PMM_POISON 0xDEADBEEFDEADBEEFULL
#endif

struct free_block {
    struct free_block *next;
};

/* DDR-882 (item 17b): one buddy free list PER NODE.
 *
 * Cross-node coalescing is prevented BY CONSTRUCTION, not by a check. The free
 * path computes buddy = addr ^ block_size(order) and calls list_remove() on
 * numa_node_of(addr)'s list. A buddy owned by a different node is simply not on
 * that list, list_remove fails, and coalescing stops. No explicit "same node?"
 * test is added, because an explicit test is a thing that can be wrong. */
static struct free_block *free_list[NUMA_MAX_NODES][PMM_MAX_ORDER];
static uint64_t free_pages;
static uint64_t total_pages;                 /* DDR-752: managed frames at init */

/* Copy-on-write per-frame reference counts (IMP-D), indexed by frame number
 * (phys >> 12) over the whole identity-mapped range [0, 1 GiB). alloc sets 1,
 * pmm_free_page decrements and frees only at 0, COW fork increments on share.
 * 262144 frames * 2 B = 512 KiB — too large for the kernel's low-memory BSS, so
 * it is allocated FROM the pool in pmm_init (NULL until then; refcounting is a
 * no-op before it exists, which is fine — nothing forks that early). */
#define PMM_NFRAMES 262144u
static uint16_t *pmm_refcount;

static inline void rc_set(uint64_t addr, unsigned order, uint16_t v) {
    if (!pmm_refcount)
        return;
    uint64_t base = addr >> PAGE_SHIFT;
    for (uint64_t i = 0; i < (1ull << order); i++)
        if (base + i < PMM_NFRAMES)
            pmm_refcount[base + i] = v;
}

static inline uint64_t block_size(unsigned order) {
    return PAGE_SIZE << order;
}

/* The free lists are shared mutable state. ADR-030 stage 1: the ADR-016
 * interrupt masking becomes a spinlock's irqsave acquire — identical semantics
 * on one CPU (IF masked across the critical section, flags restored), plus
 * cross-CPU mutual exclusion now that the APs are online (ADR-029). The
 * helpers keep their names so every call site is unchanged. */
#include "spinlock.h"
static spinlock_t g_pmm_lock = SPINLOCK_INIT;
static inline uint64_t irq_save(void) {
    return spin_lock_irqsave(&g_pmm_lock);
}
static inline void irq_restore(uint64_t f) {
    spin_unlock_irqrestore(&g_pmm_lock, f);
}

static void list_push(unsigned node, unsigned order, uint64_t addr) {
    struct free_block *b = (struct free_block *)(uintptr_t)addr;
    b->next = free_list[node][order];
    free_list[node][order] = b;
}

static uint64_t list_pop(unsigned node, unsigned order) {
    struct free_block *b = free_list[node][order];
    if (!b)
        return 0;
    free_list[node][order] = b->next;
    return (uint64_t)(uintptr_t)b;
}

/* Remove a specific address from an order's free list; 1 if found. */
static int list_remove(unsigned node, unsigned order, uint64_t addr) {
    struct free_block **pp = &free_list[node][order];
    while (*pp) {
        if ((uint64_t)(uintptr_t)(*pp) == addr) {
            *pp = (*pp)->next;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/* Post-allocation bookkeeping shared by both entry points. Caller holds the
 * lock. Factored out rather than duplicated: two copies of the OPEN-11
 * duplicate-detection check are two places for it to drift out of step. */
static void pmm_finish_alloc(uint64_t addr, unsigned order) {
    /* DDR-830 follow-up: a block just popped off the free list must have no
     * owner. If it already does, the free list contained a duplicate and we are
     * about to hand one frame to a second owner — the OPEN-11 signature. Report
     * before overwriting the evidence. */
    if (order == 0 && pmm_refcount) {
        uint64_t didx = addr >> PAGE_SHIFT;
        if (didx < PMM_NFRAMES && pmm_refcount[didx] != 0) {
            kputs("[pmm] FREE-LIST DUPLICATE addr=");
            kputhex(addr);
            kputs(" rc=");
            kputdec(pmm_refcount[didx]);
            kputs("\r\n");
        }
    }
    rc_set(addr, order, 1);             /* fresh allocation: one owner */
}

/* Try ONE node. Caller holds the lock. 0 = nothing available on this node. */
static uint64_t alloc_from_node(unsigned node, unsigned order) {
    unsigned o = order;
    while (o < PMM_MAX_ORDER && !free_list[node][o])
        o++;
    if (o >= PMM_MAX_ORDER)
        return 0;
    uint64_t addr = list_pop(node, o);
    while (o > order) {                 /* split down, freeing the upper buddy */
        o--;
        list_push(node, o, addr + block_size(o));
    }
    return addr;
}

uint64_t pmm_alloc_pages_node(unsigned node, unsigned order) {
    if (order >= PMM_MAX_ORDER || node >= NUMA_MAX_NODES)
        return 0;
    uint64_t fl = irq_save();
    uint64_t addr = alloc_from_node(node, order);
    if (!addr) {
        /* FALLBACK IS DELIBERATE. A NUMA node is a PREFERENCE, not a
         * constraint: returning 0 while free memory exists on another node
         * would turn a locality hint into an out-of-memory condition, and the
         * caller (kheap, the ELF loader, a page fault) has no way to know that
         * is what happened. Locality is an optimisation; allocation failing is
         * a correctness event. */
        for (unsigned n = 0; n < numa_node_count() && !addr; n++)
            if (n != node)
                addr = alloc_from_node(n, order);
    }
    if (addr) {
        free_pages -= (1ull << order);
        pmm_finish_alloc(addr, order);
    }
    irq_restore(fl);
    if (!addr)
        return 0;
    return addr;
}

uint64_t pmm_alloc_pages(unsigned order) {
    if (order >= PMM_MAX_ORDER)
        return 0;

    uint64_t fl = irq_save();
    uint64_t addr = 0;
    unsigned node = numa_node_of_cpu(lapic_id());
    if (node >= NUMA_MAX_NODES)
        node = 0;
    addr = alloc_from_node(node, order);
    if (!addr)
        for (unsigned n = 0; n < numa_node_count() && !addr; n++)
            if (n != node)
                addr = alloc_from_node(n, order);
    if (addr) {
        free_pages -= (1ull << order);
        pmm_finish_alloc(addr, order);
    }
    irq_restore(fl);
    return addr;                        /* 0 == out of memory at this size */
}

/* DDR-1065: returns 1 if frames were ACTUALLY RELEASED, 0 if this call only
 * dropped a reference (COW) or was rejected as a double free. ptnode_free needs
 * this to keep ptnode_in_use honest: it used to decrement unconditionally, so a
 * COW-shared page decremented TWICE (once per address space) against ONE
 * increment at ptnode_alloc, and kheap_outstanding() drifted down and wrapped.
 * Callers that ignore the value are unaffected -- void->int is source-compatible
 * and there is no warn_unused_result here, so all 132 existing call sites stand. */
int pmm_free_pages(uint64_t addr, unsigned order) {
    uint64_t fl = irq_save();
    /* COW: a single frame with other owners is only dereferenced, not freed (and
     * must NOT be poisoned — another address space still maps it). */
    if (order == 0 && pmm_refcount) {
        uint64_t idx = addr >> PAGE_SHIFT;
        if (idx < PMM_NFRAMES && pmm_refcount[idx] > 1) {
            pmm_refcount[idx]--;
            irq_restore(fl);
            return 0;                   /* DDR-1065: reference dropped, NOT released */
        }
        /* DDR-830: refcount 0 means this frame is ALREADY on the free list.
         * Absorbing the second free would push it on twice, and the allocator
         * would then hand one frame to two owners — silent, delayed, cross-
         * subsystem corruption (OPEN-11). A free list is a set; reject the
         * duplicate loudly instead of destroying that invariant. */
        if (idx < PMM_NFRAMES && pmm_refcount[idx] == 0) {
            kputs("[pmm] DOUBLE FREE rejected addr=");
            kputhex(addr);
            kputs(" caller=");
            kputhex((uint64_t)(uintptr_t)__builtin_return_address(0));
            kputs("\r\n");
            irq_restore(fl);
            return 0;                   /* DDR-1065: rejected, nothing released */
        }
    }
    rc_set(addr, order, 0);             /* actually freeing now: clear refcount(s) */
#ifdef KASAN
    {
        uint64_t *p = (uint64_t *)(uintptr_t)addr;
        uint64_t nqwords = block_size(order) / 8u;
        for (uint64_t i = 0; i < nqwords; i++)
            p[i] = PMM_POISON;
    }
#endif
    free_pages += (1ull << order);
    /* The owning node decides which list is searched — that is what makes
     * cross-node coalescing impossible without an explicit check. */
    unsigned fnode = (unsigned)numa_node_of(addr);
    if (fnode >= NUMA_MAX_NODES)
        fnode = 0;
    while (order < PMM_MAX_ORDER - 1) {
        uint64_t buddy = addr ^ block_size(order);
        if (buddy < PMM_MIN_PHYS || buddy + block_size(order) > PMM_MAX_PHYS)
            break;
        if (!list_remove(fnode, order, buddy))
            break;                      /* buddy not free -> cannot coalesce */
        if (buddy < addr)
            addr = buddy;
        order++;
    }
    list_push(fnode, order, addr);
    irq_restore(fl);
    return 1;                           /* DDR-1065: frames actually released */
}

uint64_t pmm_alloc_page(void)        { return pmm_alloc_pages(0); }
int      pmm_free_page(uint64_t a)   { return pmm_free_pages(a, 0); }
uint64_t pmm_free_page_count(void)   { return free_pages; }
uint64_t pmm_total_page_count(void)  { return total_pages; }  /* DDR-752 */

void pmm_incref(uint64_t phys) {
    uint64_t idx = phys >> PAGE_SHIFT;
    uint64_t fl = irq_save();
    if (pmm_refcount && idx < PMM_NFRAMES && pmm_refcount[idx] != 0xFFFFu)
        pmm_refcount[idx]++;
    irq_restore(fl);
}

uint16_t pmm_refcount_get(uint64_t phys) {
    uint64_t idx = phys >> PAGE_SHIFT;
    return (pmm_refcount && idx < PMM_NFRAMES) ? pmm_refcount[idx] : 0;
}

/* Carve [s,e) into maximal naturally-aligned power-of-2 blocks. */
static void add_region(uint64_t s, uint64_t e) {
    s = (s + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    e = e & ~(uint64_t)(PAGE_SIZE - 1);
    while (s < e) {
        unsigned order = PMM_MAX_ORDER - 1;
        while (order > 0) {
            uint64_t bs = block_size(order);
            if (bs <= (e - s) && (s & (bs - 1)) == 0)
                break;
            order--;
        }
        list_push(0, order, s);   /* pre-SRAT; numa_rebucket re-files */
        free_pages += (1ull << order);
        s += block_size(order);
    }
}

void pmm_init(const struct boot_info *bi) {
    for (unsigned n = 0; n < NUMA_MAX_NODES; n++)
        for (unsigned i = 0; i < PMM_MAX_ORDER; i++)
            free_list[n][i] = 0;
    free_pages = 0;

    for (uint32_t i = 0; i < bi->e820_count; i++) {
        if (bi->e820[i].type != 1)          /* type 1 = usable RAM */
            continue;
        uint64_t s = bi->e820[i].base;
        uint64_t e = bi->e820[i].base + bi->e820[i].len;
        if (s < PMM_MIN_PHYS) s = PMM_MIN_PHYS;
        if (e > PMM_MAX_PHYS) e = PMM_MAX_PHYS;
        if (s < e)
            add_region(s, e);
    }
    total_pages = free_pages;               /* DDR-752: all managed frames, before
                                             * the permanent refcount-table carve */

    /* COW refcount table: 262144 * 2 B = 512 KiB = order-7 block, taken from the
     * pool (too large for the kernel's low-memory BSS). pmm_alloc_pages runs with
     * pmm_refcount still NULL, so this permanent block carries no refcount. */
    uint64_t rc_block = pmm_alloc_pages(7);
    if (rc_block) {
        pmm_refcount = (uint16_t *)(uintptr_t)rc_block;
        for (uint64_t i = 0; i < PMM_NFRAMES; i++)
            pmm_refcount[i] = 0;
    }
#ifdef KASAN
    kputs("[pmm] poison enabled\r\n");
#endif
}

/* ---- DDR-882 (item 17b): re-bucket the free lists onto their nodes ---------
 *
 * pmm_init() runs at main.c:2195; acpi_init() at :2343. The buddy lists are
 * therefore already seeded — entirely onto node 0 — before SRAT is readable.
 * Rather than reorder two subsystems this early in boot (the change most likely
 * to produce a red that cannot be attributed), the lists are re-filed once,
 * after numa_init().
 *
 * A block may STRADDLE a node boundary. Such a block is split into its two
 * buddies and each re-processed, recursively, down to order 0 — at which point a
 * 4 KiB frame lies in exactly one node, because numa.c rejects any SRAT range
 * that is not page aligned. Bounded: at most one straddling block per node
 * boundary per order, and depth is at most PMM_MAX_ORDER.
 */
static int block_straddles(uint64_t addr, unsigned order) {
    return numa_node_of(addr) != numa_node_of(addr + block_size(order) - 1);
}

static void refile(uint64_t addr, unsigned order) {
    if (order > 0 && block_straddles(addr, order)) {
        refile(addr, order - 1);
        refile(addr + block_size(order - 1), order - 1);
        return;
    }
    unsigned n = (unsigned)numa_node_of(addr);
    if (n >= NUMA_MAX_NODES)
        n = 0;
    list_push(n, order, addr);
}

void pmm_numa_rebucket(void) {
    uint64_t fl = irq_save();
    uint64_t moved = 0;
    for (unsigned src = 0; src < NUMA_MAX_NODES; src++) {
        for (unsigned o = 0; o < PMM_MAX_ORDER; o++) {
            struct free_block *chain = free_list[src][o];
            free_list[src][o] = 0;
            while (chain) {
                uint64_t a = (uint64_t)(uintptr_t)chain;
                /* Read the link BEFORE refile() overwrites this block's first
                 * qword by pushing it onto another list. */
                chain = chain->next;
                refile(a, o);
                moved++;
            }
        }
    }
    irq_restore(fl);

    kputs("[pmm] numa rebucket blocks=");
    kputdec(moved);
    for (unsigned n = 0; n < numa_node_count(); n++) {
        uint64_t pages = 0;
        for (unsigned o = 0; o < PMM_MAX_ORDER; o++)
            for (struct free_block *b = free_list[n][o]; b; b = b->next)
                pages += (1ull << o);
        kputs(" node");
        kputdec(n);
        kputs("=");
        kputdec(pages);
    }
    kputs("\r\n");
}
