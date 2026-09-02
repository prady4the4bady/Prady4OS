/* kernel/lock_stat.c — DDR-1047: spinlock contention accounting.
 * See lock_stat.h for why this measures WAIT time and not hold time, and why
 * the storage is a side table rather than fields in spinlock_t. */
#include "lock_stat.h"
#include "spinlock.h"
#include "console.h"

static inline uint64_t ls_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

struct ls_slot {
    volatile uint64_t key;        /* lock address; 0 = free                    */
    volatile uint64_t hits;       /* contended acquisitions                    */
    volatile uint64_t wait_total; /* cycles spent waiting, summed              */
    volatile uint64_t wait_max;   /* worst single wait                         */
};

/* BSS: zero by definition, so no initialiser is owed anywhere and
 * §NON-NEGOTIABLE 10's kmalloc-does-not-zero trap does not arise. */
static struct ls_slot g_ls[LOCK_STAT_SLOTS];
static volatile uint64_t g_ls_overflow;   /* contended locks with no slot left */

/* Find or claim this lock's slot. Lock-free by necessity -- taking a lock to
 * record lock statistics would recurse. Returns NULL when the table is full,
 * and the caller counts that rather than silently attributing it elsewhere. */
static struct ls_slot *ls_slot_for(uint64_t key) {
    for (unsigned i = 0; i < LOCK_STAT_SLOTS; i++) {
        uint64_t k = __atomic_load_n(&g_ls[i].key, __ATOMIC_ACQUIRE);
        if (k == key)
            return &g_ls[i];
        if (k == 0) {
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&g_ls[i].key, &expect, key, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return &g_ls[i];
            /* Lost the race: another CPU claimed it. If it claimed it FOR THIS
             * SAME LOCK we want that slot, so re-check rather than moving on --
             * otherwise one lock could occupy two slots and both counts would
             * be half the truth. */
            if (__atomic_load_n(&g_ls[i].key, __ATOMIC_ACQUIRE) == key)
                return &g_ls[i];
        }
    }
    return 0;
}

void spin_lock_contended(void *lock) {
    spinlock_t *l = (spinlock_t *)lock;
    uint64_t t0 = ls_rdtsc();

    while (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");

    uint64_t waited = ls_rdtsc() - t0;
    struct ls_slot *s = ls_slot_for((uint64_t)(uintptr_t)l);
    if (!s) {
        __atomic_add_fetch(&g_ls_overflow, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&s->hits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->wait_total, waited, __ATOMIC_RELAXED);
    /* Relaxed max: a concurrent update can lose a sample. That is acceptable
     * for a diagnostic and is stated rather than papered over with a CAS loop
     * that would cost more than the thing being measured. */
    if (waited > __atomic_load_n(&s->wait_max, __ATOMIC_RELAXED))
        __atomic_store_n(&s->wait_max, waited, __ATOMIC_RELAXED);
}

/* Every counter is printed at full 64-bit width. kputdec takes a uint64_t
 * (console.h:12), so a narrowing cast here would buy nothing and would silently
 * print a wrong number once a count passed 2^32 -- g_sched_lock was already at
 * 1.5e6 contended acquisitions by t=5000 in the DDR-1047 measurement. */
void lock_stat_dump(void) {
    kputs("PRADYOS_LOCKSTAT overflow=");
    kputdec(__atomic_load_n(&g_ls_overflow, __ATOMIC_RELAXED));
    kputs("\r\n");
    for (unsigned i = 0; i < LOCK_STAT_SLOTS; i++) {
        uint64_t k = __atomic_load_n(&g_ls[i].key, __ATOMIC_ACQUIRE);
        if (!k)
            continue;
        uint64_t hits = __atomic_load_n(&g_ls[i].hits, __ATOMIC_RELAXED);
        if (!hits)
            continue;
        kputs("PRADYOS_LOCKSTAT lock=");
        kputhex(k);
        kputs(" hits=");
        kputdec(hits);
        kputs(" waitavg=");
        kputdec(__atomic_load_n(&g_ls[i].wait_total, __ATOMIC_RELAXED) / hits);
        kputs(" waitmax=");
        kputdec(__atomic_load_n(&g_ls[i].wait_max, __ATOMIC_RELAXED));
        kputs("\r\n");
    }
}
