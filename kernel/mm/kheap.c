/* kernel/kheap.c — slab + general kmalloc on the buddy PMM (Phase 2b).
 *
 * Design (ADR-003 follow-on):
 *   - Each slab is one PMM page, page-aligned, with a `struct slab` header at
 *     offset 0 and fixed-size objects carved after it. Objects therefore never
 *     start at a page boundary, which lets kfree distinguish slab objects
 *     (ptr & 0xFFF != 0) from whole-page "large" allocations (page-aligned).
 *   - kmalloc(size <= 2048) -> the smallest size-class cache; larger -> whole
 *     pages from the PMM, tracked in a small registry.
 *   - Dedicated caches for PCB / capability token / IPC message; page-table
 *     nodes are full zeroed pages straight from the PMM.
 *
 * KHEAP_DEBUG adds free-poisoning, double-free detection, and leak accounting.
 */
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "console.h"

#define KHEAP_DEBUG 1
#define SLAB_MAGIC  0x534C4142u          /* 'SLAB' */
#define POISON_FREE 0xDDu
/* 8-byte canary written into every FREE slab object at offset 8 (just past the
 * intrusive free-list link). Verified on allocation: a mismatch means a freed
 * object was written through (use-after-free / overflow from a neighbour). Every
 * size class is >= 16 bytes, so offset 8..15 is always inside the object. ULL
 * suffix avoids -Woverflow on the 64-bit literal. */
#define KHEAP_CANARY 0xFEEDFACEFEEDFACEULL
#define MAX_SLAB_OBJ 2048u               /* above this, allocate whole pages */
#define MAX_LARGE   256u                 /* tracked concurrent large allocs   */

struct free_obj { struct free_obj *next; };

struct slab {
    struct kmem_cache *cache;
    struct slab       *next;
    struct free_obj   *free;
    uint32_t           free_count;
    uint32_t           magic;
};

struct kmem_cache {
    const char *name;
    uint32_t    obj_size;                /* 16-byte aligned */
    uint32_t    objs_per_slab;
    struct slab *slabs;
    uint64_t    in_use;
};

#define SLAB_HDR (((sizeof(struct slab)) + 15u) & ~15u)

/* Generic size-class caches for kmalloc. */
static const uint32_t size_classes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define NUM_CLASSES (sizeof(size_classes) / sizeof(size_classes[0]))
static struct kmem_cache class_cache[NUM_CLASSES];

/* Dedicated object caches. */
static struct kmem_cache cache_pcb;      /* provisional 512 B */
static struct kmem_cache cache_cap;      /* 16 B (128-bit token) */
static struct kmem_cache cache_ipc;      /* provisional 256 B */

/* Large (whole-page) allocation registry. */
static struct { uint64_t addr; unsigned order; } large[MAX_LARGE];
static uint64_t large_in_use;
static uint64_t ptnode_in_use;

static void heap_panic(const char *msg) {
    kputs("\r\n*** KHEAP PANIC: ");
    kputs(msg);
    kputs(" ***\r\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}

static uint32_t align16(uint32_t x) { return (x + 15u) & ~15u; }

static void cache_init(struct kmem_cache *c, const char *name, uint32_t size) {
    c->name = name;
    c->obj_size = align16(size);
    c->objs_per_slab = (uint32_t)((PAGE_SIZE - SLAB_HDR) / c->obj_size);
    c->slabs = 0;
    c->in_use = 0;
}

static struct slab *cache_grow(struct kmem_cache *c) {
    uint64_t page = pmm_alloc_page();
    if (!page)
        return 0;
    struct slab *s = (struct slab *)(uintptr_t)page;
    s->cache = c;
    s->next = c->slabs;
    s->free = 0;
    s->free_count = 0;
    s->magic = SLAB_MAGIC;
    c->slabs = s;

    uint8_t *base = (uint8_t *)s + SLAB_HDR;
    for (uint32_t i = 0; i < c->objs_per_slab; i++) {
        struct free_obj *o = (struct free_obj *)(base + (size_t)i * c->obj_size);
        o->next = s->free;
#if KHEAP_DEBUG
        *(uint64_t *)((uint8_t *)o + 8) = KHEAP_CANARY;   /* obj_size >= 16 */
#endif
        s->free = o;
        s->free_count++;
    }
    return s;
}

static void *cache_alloc(struct kmem_cache *c) {
    struct slab *s = c->slabs;
    while (s && s->free_count == 0)
        s = s->next;
    if (!s) {
        s = cache_grow(c);
        if (!s)
            return 0;
    }
    struct free_obj *o = s->free;
    s->free = o->next;
#if KHEAP_DEBUG
    if (*(uint64_t *)((uint8_t *)o + 8) != KHEAP_CANARY)
        heap_panic("kalloc: free-object canary smashed (use-after-free)");
#endif
    s->free_count--;
    c->in_use++;
    return o;
}

static void cache_free(struct kmem_cache *c, void *ptr) {
    struct slab *s = (struct slab *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
#if KHEAP_DEBUG
    if (s->magic != SLAB_MAGIC)
        heap_panic("kfree: bad slab magic");
    if (s->cache != c)
        heap_panic("kfree: cache mismatch");
    for (struct free_obj *f = s->free; f; f = f->next) {
        if (f == (struct free_obj *)ptr)
            heap_panic("kfree: double free");
    }
    memset(ptr, POISON_FREE, c->obj_size);
#endif
    struct free_obj *o = (struct free_obj *)ptr;
    o->next = s->free;
#if KHEAP_DEBUG
    *(uint64_t *)((uint8_t *)o + 8) = KHEAP_CANARY;   /* re-arm after the poison fill */
#endif
    s->free = o;
    s->free_count++;
    c->in_use--;
}

static struct kmem_cache *class_for(size_t size) {
    for (unsigned i = 0; i < NUM_CLASSES; i++) {
        if (size <= size_classes[i])
            return &class_cache[i];
    }
    return 0;
}

static unsigned order_for_bytes(size_t size) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned order = 0;
    while (((uint64_t)1 << order) < pages)
        order++;
    return order;
}

void kheap_init(void) {
    for (unsigned i = 0; i < NUM_CLASSES; i++)
        cache_init(&class_cache[i], "kmalloc", size_classes[i]);
    cache_init(&cache_pcb, "pcb", 512);
    cache_init(&cache_cap, "cap", 16);
    cache_init(&cache_ipc, "ipc", 256);
    for (unsigned i = 0; i < MAX_LARGE; i++) {
        large[i].addr = 0;
        large[i].order = 0;
    }
    large_in_use = 0;
    ptnode_in_use = 0;
}

void *kmalloc(size_t size) {
    if (size == 0)
        return 0;

    if (size <= MAX_SLAB_OBJ) {
        struct kmem_cache *c = class_for(size);
        return c ? cache_alloc(c) : 0;
    }

    /* Large: whole pages from the PMM, recorded for kfree. */
    unsigned order = order_for_bytes(size);
    uint64_t addr = pmm_alloc_pages(order);
    if (!addr)
        return 0;
    for (unsigned i = 0; i < MAX_LARGE; i++) {
        if (large[i].addr == 0) {
            large[i].addr = addr;
            large[i].order = order;
            large_in_use++;
            return (void *)(uintptr_t)addr;
        }
    }
    pmm_free_pages(addr, order);          /* registry full */
    return 0;
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    if (((uintptr_t)ptr & (PAGE_SIZE - 1)) == 0) {     /* page-aligned -> large */
        uint64_t a = (uint64_t)(uintptr_t)ptr;
        for (unsigned i = 0; i < MAX_LARGE; i++) {
            if (large[i].addr == a) {
                pmm_free_pages(a, large[i].order);
                large[i].addr = 0;
                large[i].order = 0;
                large_in_use--;
                return;
            }
        }
#if KHEAP_DEBUG
        heap_panic("kfree: unknown page-aligned pointer");
#endif
        return;
    }

    struct slab *s = (struct slab *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
#if KHEAP_DEBUG
    if (s->magic != SLAB_MAGIC)
        heap_panic("kfree: pointer not from kheap");
#endif
    cache_free(s->cache, ptr);
}

void *pcb_alloc(void)    { return cache_alloc(&cache_pcb); }
void  pcb_free(void *p)  { cache_free(&cache_pcb, p); }
void *cap_alloc(void)    { return cache_alloc(&cache_cap); }
void  cap_free(void *p)  { cache_free(&cache_cap, p); }
void *ipc_alloc(void)    { return cache_alloc(&cache_ipc); }
void  ipc_free(void *p)  { cache_free(&cache_ipc, p); }

void *ptnode_alloc(void) {
    uint64_t page = pmm_alloc_page();
    if (!page)
        return 0;
    memset((void *)(uintptr_t)page, 0, PAGE_SIZE);
    ptnode_in_use++;
    return (void *)(uintptr_t)page;
}

void ptnode_free(void *p) {
    if (!p)
        return;
    pmm_free_page((uint64_t)(uintptr_t)p);
    ptnode_in_use--;
}

uint64_t kheap_outstanding(void) {
    uint64_t total = large_in_use + ptnode_in_use
                   + cache_pcb.in_use + cache_cap.in_use + cache_ipc.in_use;
    for (unsigned i = 0; i < NUM_CLASSES; i++)
        total += class_cache[i].in_use;
    return total;
}
