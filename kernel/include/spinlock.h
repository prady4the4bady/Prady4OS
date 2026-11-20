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

#ifndef LOCKDEP
#define LOCKDEP 0
#endif

#if LOCKDEP
/* DDR-1209 D2: the checking build carries the lock's class in the lock, with a
 * check byte (chk == ~cls once classed; 0/0 = not yet classed). Never in the
 * shipped kernel: there spinlock_t stays one byte (DDR-1047). */
typedef struct { volatile uint8_t v; uint8_t cls; uint8_t chk; } spinlock_t;
#else
typedef struct { volatile uint8_t v; } spinlock_t;
#endif

#define SPINLOCK_INIT { 0 }

/* DDR-1209: in the checking build the wrappers must inline, so the checker's
 * return address is the caller's acquire site, not a per-file wrapper copy. */
#if LOCKDEP
#define SPIN_INLINE static inline __attribute__((always_inline))
#else
#define SPIN_INLINE static inline
#endif

/* DDR-1047: the FAST PATH IS UNCHANGED -- one test-and-set and a branch, which
 * is no more than the original loop emitted at each call site. All contention
 * accounting lives in spin_lock_contended(), out of line, so a lock taken
 * uncontended pays nothing at all. See kernel/lock_stat.h for why that
 * matters here (an always-on cost on this primitive could perturb OPEN-2). */
void spin_lock_contended(void *lock);          /* kernel/lock_stat.c */

#if LOCKDEP
/* DDR-1209 D1: out of line in the checking build only, so the call site is the
 * return address and the order check runs before the wait (kernel/proc/lockdep.c). */
void lockdep_spin_lock(spinlock_t *l);
void lockdep_spin_unlock(spinlock_t *l);
int  lockdep_spin_trylock(spinlock_t *l);
SPIN_INLINE void spin_lock(spinlock_t *l)   { lockdep_spin_lock(l); }
SPIN_INLINE void spin_unlock(spinlock_t *l) { lockdep_spin_unlock(l); }
#else
static inline void spin_lock(spinlock_t *l) {
    if (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        spin_lock_contended(l);
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_clear(&l->v, __ATOMIC_RELEASE);
}
#endif

/* Non-blocking acquire: 1 = taken, 0 = busy. For contexts that must make
 * progress even when the lock is held — notably the trap printer, which runs
 * in EXCEPTION context and would otherwise deadlock against a line-locked
 * region interrupted by a fault on its own CPU (DDR-963 §5). */
SPIN_INLINE int spin_trylock(spinlock_t *l) {
#if LOCKDEP
    return lockdep_spin_trylock(l);
#else
    return !__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE);
#endif
}

/* IRQ-save variant: also masks local interrupts so the holder cannot be
 * preempted mid-critical-section (the ADR-016 discipline, now lock-backed). */
SPIN_INLINE uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}

SPIN_INLINE void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}
