/* user/include/psync.h — user-space mutex, semaphore, condition variable and
 * barrier on SYS_FUTEX (DDR-1193; C1.4 P3).
 *
 * Header-only and freestanding, like the rest of user/include: no libc, no
 * writable globals (user.ld), GCC __atomic builtins with explicit orders.
 * Every object defaults to the private futex key (FUTEX_PRIVATE_FLAG); the
 * *_init_shared forms leave the flag off, for objects in MAP_SHARED memory
 * (the shared key, DDR-1181 A1.2).
 *
 * Gated by smoke-thread arms SA, SW, SB, SC, SH and SD (DDR-1193 §4 and
 * Amendment 1). Emulated (QEMU TCG) only; not verified on real hardware. The
 * memory-order reasoning rests on x86-64 TSO; it is not tested on a weakly
 * ordered CPU.
 *
 * Not here (DDR-1193 §3 and Amendment 1 item 6): timed waits, robust and PI
 * mutexes, rwlocks, requeue. pmutex is not FIFO (a running thread can barge),
 * and pcond_broadcast wakes every waiter to contend on the mutex.
 *
 * Return values follow pradyos.h: raw kernel values, no errno.
 */
#ifndef PSYNC_H
#define PSYNC_H

#include <stdint.h>
#include "pradyos.h"

#define PS_FUTEX_WAIT     0
#define PS_FUTEX_WAKE     1
#define PS_FUTEX_PRIVATE  128
#define PS_WAKE_ALL       0x7FFFFFFF

/* SYS_FUTEX(uaddr, op, val, timespec *) -- the timeout is in R10. P3 never
 * passes one (Amendment 1 item 6). */
static inline long ps_futex(volatile uint32_t *w, uint32_t op, uint32_t val) {
    long r;
    register long r10 __asm__("r10") = 0;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"((long)SYS_FUTEX), "D"(w), "S"((long)op), "d"((long)val), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---- pmutex: Drepper, "Futexes Are Tricky", mutex 3 ------------------ */
/* st: 0 free, 1 held, 2 held and a waiter may be asleep. A thread sets 2
 * before it sleeps, so unlock only makes a syscall when it saw 2, and a wake
 * cannot be lost: FUTEX_WAIT compares st == 2 under the bucket lock. */
typedef struct {
    volatile uint32_t st;
    uint32_t priv;                /* PS_FUTEX_PRIVATE, or 0 for shared memory */
#ifdef PSYNC_STATS
    volatile uint32_t waits;      /* probe only: futex_wait calls in the slow path */
#endif
} pmutex;

static inline void pmutex_init(pmutex *m) {
    m->st = 0; m->priv = PS_FUTEX_PRIVATE;
#ifdef PSYNC_STATS
    m->waits = 0;
#endif
}
static inline void pmutex_init_shared(pmutex *m) { pmutex_init(m); m->priv = 0; }

static inline void pmutex_lock(pmutex *m) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&m->st, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;                                       /* uncontended: 0 -> 1 */
    if (c != 2)
        c = __atomic_exchange_n(&m->st, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
#ifdef PSYNC_STATS
        __atomic_add_fetch(&m->waits, 1, __ATOMIC_RELAXED);
#endif
        ps_futex(&m->st, PS_FUTEX_WAIT | m->priv, 2);
        c = __atomic_exchange_n(&m->st, 2, __ATOMIC_ACQUIRE);
    }
}

static inline void pmutex_unlock(pmutex *m) {
    if (__atomic_exchange_n(&m->st, 0, __ATOMIC_RELEASE) == 2)
        ps_futex(&m->st, PS_FUTEX_WAKE | m->priv, 1);
}

/* ---- psem: counting semaphore ----------------------------------------- */
/* nwait++ and the FUTEX_WAIT compare on val, against post's val++ and its
 * load of nwait, are both sequentially consistent (x86 LOCK XADD): either the
 * poster sees a waiter and wakes, or the waiter's compare sees val > 0. */
typedef struct {
    volatile uint32_t val;
    volatile uint32_t nwait;
    uint32_t priv;
} psem;

static inline void psem_init(psem *s, uint32_t v) { s->val = v; s->nwait = 0; s->priv = PS_FUTEX_PRIVATE; }
static inline void psem_init_shared(psem *s, uint32_t v) { psem_init(s, v); s->priv = 0; }

static inline void psem_wait(psem *s) {
    for (;;) {
        uint32_t v = __atomic_load_n(&s->val, __ATOMIC_RELAXED);
        while (v > 0)
            if (__atomic_compare_exchange_n(&s->val, &v, v - 1, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
        __atomic_add_fetch(&s->nwait, 1, __ATOMIC_SEQ_CST);
        ps_futex(&s->val, PS_FUTEX_WAIT | s->priv, 0);
        __atomic_sub_fetch(&s->nwait, 1, __ATOMIC_SEQ_CST);
    }
}

static inline void psem_post(psem *s) {
    __atomic_add_fetch(&s->val, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s->nwait, __ATOMIC_SEQ_CST))
        ps_futex(&s->val, PS_FUTEX_WAKE | s->priv, 1);
}

/* ---- pcond: sequence-counter condition variable ----------------------- */
/* seq is read BEFORE the mutex is released, so a signal that lands between
 * the unlock and the sleep changes seq and FUTEX_WAIT returns -EAGAIN instead
 * of sleeping through it. The relock writes 2, never 1: a write of 1 could
 * overwrite the 2 a sleeping mutex waiter set, and the next unlock would skip
 * its wake (DDR-1193 Amendment 1 item 5). */
typedef struct {
    volatile uint32_t seq;
    uint32_t priv;
} pcond;

static inline void pcond_init(pcond *c) { c->seq = 0; c->priv = PS_FUTEX_PRIVATE; }
static inline void pcond_init_shared(pcond *c) { pcond_init(c); c->priv = 0; }

/* Returns the FUTEX_WAIT result: 0 after a wake, -EAGAIN if seq moved first,
 * -EINTR on a handled signal. Callers re-test their predicate either way. */
static inline long pcond_wait(pcond *c, pmutex *m) {
    uint32_t s = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
    pmutex_unlock(m);
    long r = ps_futex(&c->seq, PS_FUTEX_WAIT | c->priv, s);
    while (__atomic_exchange_n(&m->st, 1, __ATOMIC_ACQUIRE) != 0)   /* MUTANT Y6: relock writes 1 */
        ps_futex(&m->st, PS_FUTEX_WAIT | m->priv, 1);
    return r;
}

static inline void pcond_signal(pcond *c) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    ps_futex(&c->seq, PS_FUTEX_WAKE | c->priv, 1);
}

static inline void pcond_broadcast(pcond *c) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    ps_futex(&c->seq, PS_FUTEX_WAKE | c->priv, PS_WAKE_ALL);
}

/* ---- pbarrier: generation-counted barrier ----------------------------- */
/* gen is read BEFORE arriving: read after, the last arrival could bump it
 * first and this thread would wait for the next generation. The last arrival
 * resets count before it publishes the new gen (release), so a thread that
 * sees the new gen (acquire) also sees count == 0. */
typedef struct {
    volatile uint32_t count;
    volatile uint32_t gen;
    uint32_t n;
    uint32_t priv;
} pbarrier;

static inline void pbarrier_init(pbarrier *b, uint32_t n) { b->count = 0; b->gen = 0; b->n = n; b->priv = PS_FUTEX_PRIVATE; }
static inline void pbarrier_init_shared(pbarrier *b, uint32_t n) { pbarrier_init(b, n); b->priv = 0; }

/* Returns 1 in exactly one thread per generation (the last to arrive). */
static inline int pbarrier_wait(pbarrier *b) {
    uint32_t g = __atomic_load_n(&b->gen, __ATOMIC_ACQUIRE);
    if (__atomic_add_fetch(&b->count, 1, __ATOMIC_ACQ_REL) == b->n) {
        __atomic_store_n(&b->count, 0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&b->gen, 1, __ATOMIC_RELEASE);
        ps_futex(&b->gen, PS_FUTEX_WAKE | b->priv, PS_WAKE_ALL);
        return 1;
    }
    while (__atomic_load_n(&b->gen, __ATOMIC_ACQUIRE) == g)
        ps_futex(&b->gen, PS_FUTEX_WAIT | b->priv, g);
    return 0;
}

#endif /* PSYNC_H */
