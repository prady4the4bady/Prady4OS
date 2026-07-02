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

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_clear(&l->v, __ATOMIC_RELEASE);
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
