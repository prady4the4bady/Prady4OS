/* kernel/pmm.c — buddy physical frame allocator (ADR-003), Phase 2b.
 *
 * Free blocks are tracked with intrusive singly-linked lists, one per order;
 * the link lives in the free frame itself (valid because managed RAM is
 * identity-mapped). free() coalesces with the XOR-buddy when it is also free.
 * Buddy lookup on free is currently a linear scan of the order's free list —
 * correct and simple; a per-block state bitmap is the obvious later speedup.
 */
#include "pmm.h"

#define PMM_MIN_PHYS 0x01000000ull   /* 16 MiB: above kernel/bootloader/BIOS */
#define PMM_MAX_PHYS 0x40000000ull   /* 1 GiB: identity-map limit (ADR-005)   */

struct free_block {
    struct free_block *next;
};

static struct free_block *free_list[PMM_MAX_ORDER];
static uint64_t free_pages;

static inline uint64_t block_size(unsigned order) {
    return PAGE_SIZE << order;
}

/* The free lists are shared mutable state. With preemptive multitasking, two
 * threads can call pmm_alloc/free concurrently; without mutual exclusion they
 * race the intrusive lists and can hand out the same page twice. On this
 * single core, masking interrupts around the critical section is sufficient
 * (a real spinlock arrives with SMP/APIC). Save+restore the flag so callers
 * that are already in a cli region stay masked on return. */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
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
    }
    irq_restore(fl);
    return addr;                        /* 0 == out of memory at this size */
}

void pmm_free_pages(uint64_t addr, unsigned order) {
    uint64_t fl = irq_save();
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
}
