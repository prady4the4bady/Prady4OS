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
    volatile uint64_t hits;       /* waits COMPLETED (spin) / yield-waits done */
    volatile uint64_t wait_total; /* cycles spent waiting, summed (spin only)  */
    volatile uint64_t wait_max;   /* worst single wait (spin only)             */
    /* DDR-1060: waits IN PROGRESS RIGHT NOW. This is the field that makes the
     * instrument able to see the case it exists for. Everything above is
     * written only AFTER a lock is acquired, so a CPU that never acquires --
     * the wedged one -- contributes nothing to any of it; on the freeze path,
     * where this table is printed, those counters describe only the CPUs that
     * were fine. A frozen CPU leaves a permanent +1 here, on exactly the lock
     * it is stuck on, and that is the answer DDR-1006 §7 asked for.
     *
     * Per-LOCK and not per-CPU on purpose: a per-CPU `waiting_on` needs to know
     * which CPU is running, and both routes to that are documented hazards on
     * this very path -- this_cpu() reads %gs:0, and DDR-1010 caught a broken
     * SWAPGS discipline as one of OPEN-2's own producers, while lapic_id() is
     * invalid pre-LAPIC, which is why DDR-1055 refused a per-CPU guard for the
     * console. The [apfreeze] line already prints cpu=/rip=/bt=, so naming the
     * lock rather than the CPU loses nothing at the point of use. */
    volatile uint64_t waiters;
    /* 0 = spin (cycles are meaningful) | 1 = yield (they are NOT -- see the
     * dump, which gives the two kinds different LINE SHAPES rather than a
     * shared column, because a spin wait is cycles this CPU burned and a yield
     * wait is wall time during which it ran other threads. DDR-1047 left
     * mnt_lock out for exactly that reason and the boundary is kept. */
    volatile uint8_t  kind;
};

/* BSS: zero by definition, so no initialiser is owed anywhere and
 * §NON-NEGOTIABLE 10's kmalloc-does-not-zero trap does not arise. */
static struct ls_slot g_ls[LOCK_STAT_SLOTS];
static volatile uint64_t g_ls_overflow;   /* contended locks with no slot left */

/* Find or claim this lock's slot. Lock-free by necessity -- taking a lock to
 * record lock statistics would recurse. Returns NULL when the table is full,
 * and the caller counts that rather than silently attributing it elsewhere. */
static struct ls_slot *ls_slot_for(uint64_t key, uint8_t kind) {
    for (unsigned i = 0; i < LOCK_STAT_SLOTS; i++) {
        uint64_t k = __atomic_load_n(&g_ls[i].key, __ATOMIC_ACQUIRE);
        if (k == key)
            return &g_ls[i];
        if (k == 0) {
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&g_ls[i].key, &expect, key, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&g_ls[i].kind, kind, __ATOMIC_RELAXED);
                return &g_ls[i];
            }
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

/* DDR-1060: THE SLOT IS CLAIMED BEFORE THE SPIN, NOT AFTER.
 *
 * Pre-fix this function spun first and recorded second, so the only CPUs it
 * could ever account for were the ones that got the lock. That made it blind to
 * the wedged AP -- the single case lock_stat.h names as its purpose. Claiming
 * the slot up front costs the contended path a bounded scan of <=32 slots
 * before it starts spinning; THE FAST PATH IS STILL UNTOUCHED (one test-and-set
 * plus a branch, spin_lock() in spinlock.h), which is the property DDR-1047
 * refused to give up. */
void spin_lock_contended(void *lock) {
    spinlock_t *l = (spinlock_t *)lock;
    struct ls_slot *s = ls_slot_for((uint64_t)(uintptr_t)l, LS_KIND_SPIN);
    if (!s)
        __atomic_add_fetch(&g_ls_overflow, 1, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&s->waiters, 1, __ATOMIC_RELAXED);

    uint64_t t0 = ls_rdtsc();

    while (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");

    uint64_t waited = ls_rdtsc() - t0;
    if (!s)
        return;
    __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->hits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->wait_total, waited, __ATOMIC_RELAXED);
    /* Relaxed max: a concurrent update can lose a sample. That is acceptable
     * for a diagnostic and is stated rather than papered over with a CAS loop
     * that would cost more than the thing being measured. */
    if (waited > __atomic_load_n(&s->wait_max, __ATOMIC_RELAXED))
        __atomic_store_n(&s->wait_max, waited, __ATOMIC_RELAXED);
}

/* DDR-1060 §5: the yield-wait pair, for sleep-mutexes that are not spinlock_t.
 * mnt_lock (vfs.c) is a bare busy byte spun on with yield(), and it is the lock
 * DDR-994 and PRE_LAUNCH_CHECKLIST §4.11 name as the prime suspect on OPEN-1
 * route 1's path -- the one lock this table could not see.
 *
 * It contributes to `waiters` and to `hits`, and to NOTHING ELSE. No cycles are
 * recorded, because a yield wait is wall time during which this CPU ran other
 * threads while a spin wait is cycles it burned; DDR-1047 kept mnt_lock out
 * rather than put both in one waitavg column, and that boundary is preserved by
 * giving the two kinds different LINE SHAPES in the dump. `waiters` is a
 * dimensionless count, which is why it alone can carry both. */
void lock_wait_begin(void *lock) {
    struct ls_slot *s = ls_slot_for((uint64_t)(uintptr_t)lock, LS_KIND_YIELD);
    if (!s) {
        __atomic_add_fetch(&g_ls_overflow, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&s->waiters, 1, __ATOMIC_RELAXED);
}

void lock_wait_end(void *lock) {
    struct ls_slot *s = ls_slot_for((uint64_t)(uintptr_t)lock, LS_KIND_YIELD);
    if (!s)
        return;
    __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->hits, 1, __ATOMIC_RELAXED);
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
        uint64_t wtrs = __atomic_load_n(&g_ls[i].waiters, __ATOMIC_RELAXED);
        /* DDR-1060 §3, the OTHER half of the same defect: pre-fix this said
         * `if (!hits) continue;`. A lock whose only interaction is a CPU STUCK
         * waiting on it has hits == 0 -- nobody ever completed an acquisition --
         * so the printer would have skipped the one line worth printing. Both
         * halves have to move together or the fix is decorative. */
        if (!hits && !wtrs)
            continue;
        /* Two kinds, two LINE SHAPES, never one column carrying both units. */
        if (__atomic_load_n(&g_ls[i].kind, __ATOMIC_RELAXED) == LS_KIND_YIELD) {
            kputs("PRADYOS_LOCKSTAT yield lock=");
            kputhex(k);
            kputs(" waits=");
            kputdec(hits);
            kputs(" waiters=");
            kputdec(wtrs);
            kputs("\r\n");
            continue;
        }
        kputs("PRADYOS_LOCKSTAT lock=");
        kputhex(k);
        kputs(" hits=");
        kputdec(hits);
        kputs(" waitavg=");
        /* hits can be 0 here now (a frozen waiter and no completed acquire), so
         * the divisor is guarded -- pre-fix it could not be reached with 0. */
        kputdec(hits ? __atomic_load_n(&g_ls[i].wait_total, __ATOMIC_RELAXED) / hits : 0);
        kputs(" waitmax=");
        kputdec(__atomic_load_n(&g_ls[i].wait_max, __ATOMIC_RELAXED));
        kputs(" waiters=");
        kputdec(wtrs);
        kputs("\r\n");
    }
}
