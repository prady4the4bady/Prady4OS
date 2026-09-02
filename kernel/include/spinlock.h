/* kernel/include/spinlock.h — test-and-set spinlock (ADR-029).
 *
 * The cross-CPU building block for SMP. Stage B uses it only to serialize the
 * APs' boot announcements; the follow-on scheduling ADR applies it to the
 * shared subsystems (PMM/console/scheduler) when APs start doing real work —
 * until then ADR-016's interrupt masking remains the single-scheduling-CPU
 * contract. `pause` in the spin keeps the wait polite to the sibling thread.
 */
#pragma once
#include <stdint.h>

typedef struct { volatile uint8_t v; } spinlock_t;

#define SPINLOCK_INIT { 0 }

/* DDR-1047: the FAST PATH IS UNCHANGED -- one test-and-set and a branch, which
 * is no more than the original loop emitted at each call site. All contention
 * accounting lives in spin_lock_contended(), out of line, so a lock taken
 * uncontended pays nothing at all. See kernel/lock_stat.h for why that
 * matters here (an always-on cost on this primitive could perturb OPEN-2). */
void spin_lock_contended(void *lock);          /* kernel/lock_stat.c */

static inline void spin_lock(spinlock_t *l) {
    if (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        spin_lock_contended(l);
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_clear(&l->v, __ATOMIC_RELEASE);
}

/* Non-blocking acquire: 1 = taken, 0 = busy. For contexts that must make
 * progress even when the lock is held — notably the trap printer, which runs
 * in EXCEPTION context and would otherwise deadlock against a line-locked
 * region interrupted by a fault on its own CPU (DDR-963 §5). */
static inline int spin_trylock(spinlock_t *l) {
    return !__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE);
}

/* IRQ-save variant: also masks local interrupts so the holder cannot be
 * preempted mid-critical-section (the ADR-016 discipline, now lock-backed). */
static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}
