/* kernel/proc/futex.c — futex wait/wake (DDR-1181 Amendment 1, C1.4 P2).
 *
 * A waiter is a stack object hashed by its key into one of FX_BUCKETS lists.
 * WAIT reads the user word and enqueues under the bucket lock; WAKE takes the
 * same lock after the waker changed the word, so a wake is never lost (A1.3).
 *
 * Keys (A1.2): (mm, uaddr) for a private futex or a page that is not shared
 * memory; (frame | 1, page offset) for a PTE_SW_SHMEM page without
 * FUTEX_PRIVATE_FLAG, so processes that map the same frame meet.
 *
 * The page tables are walked under the mm lock: munmap frees empty tables, so
 * an unlocked walk could follow a freed one. The word itself is read through
 * the frame's identity mapping under the bucket lock -- never a user fault
 * while a spinlock is held.
 */
#include "futex.h"
#include "sched.h"
#include "mm.h"
#include "vmm.h"
#include "irq.h"          /* g_ticks */
#include "signal.h"
#include "spinlock.h"
#include "syscall.h"
#include "uaccess.h"
#include "errno.h"

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128
#define FX_BUCKETS          64
#define NS_PER_TICK         10000000L      /* 100 Hz */
#define FX_ADDR             0x000FFFFFFFFFF000ull   /* PTE frame bits */

struct fx_waiter {
    uint64_t          k1, k2;
    struct tcb       *t;
    struct fx_waiter *next;
    volatile int      woken;
};

struct fx_bucket {
    spinlock_t        lock;
    struct fx_waiter *head;
};

static struct fx_bucket g_fx[FX_BUCKETS];   /* BSS: unlocked, empty */

static struct fx_bucket *bucket(uint64_t k1, uint64_t k2) {
    uint64_t h = (k1 ^ (k2 * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull;
    return &g_fx[(h >> 58) & (FX_BUCKETS - 1)];
}

/* The key and the word's physical address for `uaddr` in `t`. Caller holds
 * the mm lock. 0, -EINVAL (unaligned) or -EFAULT (not a present user page). */
static int fx_key(struct tcb *t, uint64_t uaddr, int priv,
                  uint64_t *k1, uint64_t *k2, uint64_t *phys) {
    if (uaddr & 3)
        return -EINVAL;
    if (uaddr < VMM_USER_MIN || uaddr >= VMM_USER_MAX || !t->mm)
        return -EFAULT;
    uint64_t pte = vmm_resolve_pte(t->cr3, uaddr);
    if (!(pte & VMM_PRESENT) || !(pte & VMM_USER) || (pte & PTE_SW_DYING))
        return -EFAULT;
    uint64_t frame = pte & FX_ADDR;
    *phys = frame + (uaddr & 0xFFF);
    if (!priv && (pte & PTE_SW_SHMEM)) {
        *k1 = frame | 1;               /* a frame is page-aligned: bit 0 is free */
        *k2 = uaddr & 0xFFF;
    } else {
        *k1 = (uint64_t)(uintptr_t)t->mm;
        *k2 = uaddr;
    }
    return 0;
}

static void fx_unlink(struct fx_bucket *b, struct fx_waiter *w) {
    for (struct fx_waiter **pp = &b->head; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; return; }
}

static long fx_wake_locked_key(uint64_t k1, uint64_t k2, long n) {
    struct fx_bucket *b = bucket(k1, k2);
    long woke = 0;
    uint64_t fl = spin_lock_irqsave(&b->lock);
    struct fx_waiter **pp = &b->head;
    (void)n;
    while (*pp) {
        struct fx_waiter *w = *pp;
        if (w->k1 == k1 && w->k2 == k2) {
            *pp = w->next;
            w->woken = 1;
            sched_unblock(w->t);
            woke++;
        } else {
            pp = &w->next;
        }
    }
    spin_unlock_irqrestore(&b->lock, fl);
    return woke;
}

static long fx_wake(struct tcb *t, uint64_t uaddr, long n, int priv) {
    uint64_t k1, k2, phys;
    mm_lock(t);
    int rc = fx_key(t, uaddr, priv, &k1, &k2, &phys);
    mm_unlock(t);
    if (rc < 0)
        return rc;
    return n > 0 ? fx_wake_locked_key(k1, k2, n) : 0;
}

long futex_wake_addr(struct tcb *t, uint64_t uaddr, long n) {
    return fx_wake(t, uaddr, n, 0);
}

static long fx_wait(struct tcb *t, uint64_t uaddr, uint32_t val, int priv,
                    int timed, uint64_t ticks) {
    uint64_t k1, k2, phys;
    mm_lock(t);
    int rc = fx_key(t, uaddr, priv, &k1, &k2, &phys);
    if (rc < 0) {
        mm_unlock(t);
        return rc;
    }
    struct fx_bucket *b = bucket(k1, k2);
    struct fx_waiter w = { k1, k2, t, 0, 0 };
    uint64_t fl = spin_lock_irqsave(&b->lock);
    uint32_t cur = __atomic_load_n((volatile uint32_t *)(uintptr_t)phys, __ATOMIC_SEQ_CST);
    mm_unlock(t);                      /* kmutex_unlock never sleeps */
    if (cur != val) {
        spin_unlock_irqrestore(&b->lock, fl);
        return -EAGAIN;
    }
    if (timed && ticks == 0) {
        spin_unlock_irqrestore(&b->lock, fl);
        return -ETIMEDOUT;
    }
    w.next = b->head;
    b->head = &w;
    __atomic_store_n(&t->fx_lock, &b->lock, __ATOMIC_SEQ_CST);
    uint64_t deadline = g_ticks + ticks;
    long ret;
    for (;;) {
        if (w.woken) { ret = 0; break; }
        if (signal_interruptible(t) || signal_kill_pending(t)) { ret = -EINTR; break; }
        if (timed) {
            uint64_t now = g_ticks;
            if (now >= deadline) { ret = -ETIMEDOUT; break; }
            sched_block_timeout(&b->lock, &w.woken, deadline - now);
        } else {
            sched_block_on(&b->lock);
        }
    }
    if (!w.woken)
        fx_unlink(b, &w);
    __atomic_store_n(&t->fx_lock, (spinlock_t *)0, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&b->lock, fl);
    return ret;
}

void futex_kick(struct tcb *t) {
    spinlock_t *lk = __atomic_load_n(&t->fx_lock, __ATOMIC_SEQ_CST);
    if (!lk)
        return;
    uint64_t fl = spin_lock_irqsave(lk);
    if (__atomic_load_n(&t->fx_lock, __ATOMIC_SEQ_CST) == lk)
        sched_unblock(t);              /* its loop sees the signal and leaves */
    spin_unlock_irqrestore(lk, fl);
}

struct fx_timespec { int64_t tv_sec, tv_nsec; };

static long sys_futex(long a_uaddr, long a_op, long a_val, long a_ts, long a5, long a6) {
    (void)a5; (void)a6;
    struct tcb *t = current_thread;
    int priv = (a_op & FUTEX_PRIVATE_FLAG) != 0;
    long op = a_op & ~(long)FUTEX_PRIVATE_FLAG;
    if (op == FUTEX_WAKE)
        return fx_wake(t, (uint64_t)a_uaddr, a_val, priv);
    if (op != FUTEX_WAIT)
        return -ENOSYS;
    int timed = 0;
    uint64_t ticks = 0;
    if (a_ts) {
        struct fx_timespec ts;
        if (copyin(&ts, (const void __user *)(uintptr_t)a_ts, sizeof ts) < 0)
            return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
            return -EINVAL;
        if (ts.tv_sec > 100000000L)
            ts.tv_sec = 100000000L;    /* ~3 years: no tick overflow */
        timed = 1;
        ticks = (uint64_t)ts.tv_sec * 100u +
                (uint64_t)((ts.tv_nsec + NS_PER_TICK - 1) / NS_PER_TICK);  /* never early */
    }
    return fx_wait(t, (uint64_t)a_uaddr, (uint32_t)a_val, priv, timed, ticks);
}

void futex_register(void) {
    syscall_register(SYS_FUTEX, sys_futex);
}
