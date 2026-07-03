/* kernel/pmm.c — buddy physical frame allocator (ADR-003), Phase 2b.
 *
 * Free blocks are tracked with intrusive singly-linked lists, one per order;
 * the link lives in the free frame itself (valid because managed RAM is
 * identity-mapped). free() coalesces with the XOR-buddy when it is also free.
 * Buddy lookup on free is currently a linear scan of the order's free list —
 * correct and simple; a per-block state bitmap is the obvious later speedup.
 */
#include "pmm.h"
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

static struct free_block *free_list[PMM_MAX_ORDER];
static uint64_t free_pages;

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

static void list_push(unsigned order, uint64_t addr) {
    struct free_block *b = (struct free_block *)(uintptr_t)addr;
    b->next = free_list[order];
    free_list[order] = b;
}

static uint64_t list_pop(unsigned order) {
    struct free_block *b = free_list[order];
    if (!b)
        return 0;
    free_list[order] = b->next;
    return (uint64_t)(uintptr_t)b;
}

/* Remove a specific address from an order's free list; 1 if found. */
static int list_remove(unsigned order, uint64_t addr) {
    struct free_block **pp = &free_list[order];
    while (*pp) {
        if ((uint64_t)(uintptr_t)(*pp) == addr) {
            *pp = (*pp)->next;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

uint64_t pmm_alloc_pages(unsigned order) {
    if (order >= PMM_MAX_ORDER)
        return 0;

    uint64_t fl = irq_save();
    uint64_t addr = 0;
    unsigned o = order;
    while (o < PMM_MAX_ORDER && !free_list[o])
        o++;
    if (o < PMM_MAX_ORDER) {
        addr = list_pop(o);
        while (o > order) {             /* split down, freeing the upper buddy */
            o--;
            list_push(o, addr + block_size(o));
        }
        free_pages -= (1ull << order);
        rc_set(addr, order, 1);         /* fresh allocation: one owner */
    }
    irq_restore(fl);
    return addr;                        /* 0 == out of memory at this size */
}

void pmm_free_pages(uint64_t addr, unsigned order) {
    uint64_t fl = irq_save();
    /* COW: a single frame with other owners is only dereferenced, not freed (and
     * must NOT be poisoned — another address space still maps it). */
    if (order == 0 && pmm_refcount) {
        uint64_t idx = addr >> PAGE_SHIFT;
        if (idx < PMM_NFRAMES && pmm_refcount[idx] > 1) {
            pmm_refcount[idx]--;
            irq_restore(fl);
            return;
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
    while (order < PMM_MAX_ORDER - 1) {
        uint64_t buddy = addr ^ block_size(order);
        if (buddy < PMM_MIN_PHYS || buddy + block_size(order) > PMM_MAX_PHYS)
            break;
        if (!list_remove(order, buddy))
            break;                      /* buddy not free -> cannot coalesce */
        if (buddy < addr)
            addr = buddy;
        order++;
    }
    list_push(order, addr);
    irq_restore(fl);
}

uint64_t pmm_alloc_page(void)        { return pmm_alloc_pages(0); }
void     pmm_free_page(uint64_t a)   { pmm_free_pages(a, 0); }
uint64_t pmm_free_page_count(void)   { return free_pages; }

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
        list_push(order, s);
        free_pages += (1ull << order);
        s += block_size(order);
    }
}

void pmm_init(const struct boot_info *bi) {
    for (unsigned i = 0; i < PMM_MAX_ORDER; i++)
        free_list[i] = 0;
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
