/* kernel/proc/rcu.c — kernel RCU, one grace-period scheme (DDR-1200, C1.4 P5a).
 * See rcu.h for the rules. QEMU only, not verified on real hardware. */
#include "rcu.h"
#include "percpu.h"
#include "console.h"
#include "sched.h"
#include "kheap.h"
#include "irq.h"
#include "errno.h"

/* Per-CPU read-section nesting, indexed by cpu_idx. Only this CPU writes its
 * own slot, with IF clear, so a plain access is enough; it is read only by the
 * rule checks on the same CPU. */
static uint32_t g_rcu_nest[PERCPU_MAX];
static uint64_t g_rcu_stalls;
static uint32_t g_rcu_fail_said;     /* one "RCU FAIL" line per kind, not a flood */

static inline uint32_t cpu_slot(void) {
    struct percpu *pc = this_cpu();
    return pc ? pc->cpu_idx : 0;
}

void rcu_fail_once(uint32_t bit, const char *what) {
    if (__atomic_fetch_or(&g_rcu_fail_said, bit, __ATOMIC_RELAXED) & bit)
        return;
    kline k; kline_init(&k);
    kline_s(&k, "RCU FAIL: "); kline_s(&k, what);
    kline_s(&k, " cpu="); kline_d(&k, cpu_slot());
    kline_s(&k, "\r\n"); kline_emit(&k);
}

uint64_t rcu_read_lock(void) {
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    g_rcu_nest[cpu_slot()]++;
    __asm__ volatile("" ::: "memory");
    return fl;
}

void rcu_read_unlock(uint64_t fl) {
    __asm__ volatile("" ::: "memory");
    uint32_t c = cpu_slot();
    if (g_rcu_nest[c] == 0)
        rcu_fail_once(1u, "rcu_read_unlock without rcu_read_lock");
    else
        g_rcu_nest[c]--;
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
}

uint32_t rcu_nesting(void) {
    return g_rcu_nest[cpu_slot()];
}

uint64_t rcu_stalls(void) {
    return __atomic_load_n(&g_rcu_stalls, __ATOMIC_RELAXED);
}

uint32_t rcu_fail_bits(void) {
    return __atomic_load_n(&g_rcu_fail_said, __ATOMIC_RELAXED);
}

/* Rule 1. schedule() calls this before it switches. A thread switched out
 * inside a read section would leave this CPU's count raised for whoever runs
 * next, and its section would span ticks it did not see. */
void rcu_note_switch(void) {
    if (g_rcu_nest[cpu_slot()] != 0)
        rcu_fail_once(2u, "switch inside a read section");
}

void synchronize_rcu(void) {
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    if (!(fl & 0x200u) || rcu_nesting() != 0)
        rcu_fail_once(4u, "synchronize_rcu with IF clear or inside a read section");

    /* §3 step 1: the caller's unlink is visible to every CPU before the
     * snapshot is taken. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Step 2: snapshot every ticking CPU but this one. This CPU is outside
     * every read section right now (IF is set here, rule 2), so a section it
     * runs later starts after the unlink. Moving to another CPU after this
     * point changes nothing: that CPU is in the snapshot. */
    struct percpu *self = this_cpu();
    uint64_t snap[PERCPU_MAX];
    uint8_t  want[PERCPU_MAX];      /* waited on: every ticking CPU but this one */
    uint8_t  have[PERCPU_MAX];      /* snapshotted: every ticking CPU (stall clock) */
    for (uint32_t i = 0; i < PERCPU_MAX; i++) {
        struct percpu *p = percpu_get(i);
        want[i] = 0;
        have[i] = 0;
        snap[i] = 0;
        if (!p || !p->present ||
            !__atomic_load_n(&p->shoot_ready, __ATOMIC_ACQUIRE))
            continue;
        snap[i] = __atomic_load_n(&p->ticks, __ATOMIC_ACQUIRE);
        have[i] = 1;
        if (p != self)
            want[i] = 1;
    }

    /* Step 3: wait until every snapshotted counter has moved. The stall clock
     * is the furthest any ticking CPU has advanced since the snapshot, so it
     * runs whichever CPU this thread is on, and across moves between CPUs
     * (CodeRabbit on #123: a per-CPU delta reset on every move never grew). */
    int warned = 0;
    for (;;) {
        int left = 0;
        uint32_t first_left = 0;
        for (uint32_t i = 0; i < PERCPU_MAX; i++) {
            if (!want[i])
                continue;
            struct percpu *p = percpu_get(i);
            if (__atomic_load_n(&p->ticks, __ATOMIC_ACQUIRE) != snap[i]) {
                want[i] = 0;
                continue;
            }
            if (!left)
                first_left = i;
            left++;
        }
        if (!left)
            break;

        uint64_t waited = 0;
        for (uint32_t i = 0; i < PERCPU_MAX; i++) {
            if (!have[i])
                continue;
            uint64_t d = __atomic_load_n(&percpu_get(i)->ticks, __ATOMIC_RELAXED) - snap[i];
            if (d > waited)
                waited = d;
        }
        if (!warned && waited >= RCU_STALL_TICKS) {
            warned = 1;
            __atomic_fetch_add(&g_rcu_stalls, 1, __ATOMIC_RELAXED);
            kline k; kline_init(&k);
            kline_s(&k, "[rcu] stall cpu="); kline_d(&k, first_left);
            kline_s(&k, " waited="); kline_d(&k, waited);
            kline_s(&k, "\r\n"); kline_emit(&k);
        }
        yield();
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#if RCU_TORTURE
/* DDR-1200 §7, arms RA, RB, RC: one reader kernel thread pinned to each ticking
 * CPU, a writer that swaps a published node and frees the old one after a
 * grace period. A reader holds the node for about RCU_T_HOLD_TICKS ticks of
 * TSC time with IF clear (RB: longer than a tick), and checks before and after
 * that the node is the one it read. A node freed under a reader is poisoned
 * with 0xDD first, and its slab slot is usually handed straight back to the
 * next swap, so a reused node shows up as a changed generation too. */
#define RCU_T_MAGIC         0x52435530544F5254ull   /* "RCU0TORT" */
#define RCU_T_POISON        0xDDDDDDDDDDDDDDDDull
#define RCU_T_HOLD_TICKS    3u                       /* in half-ticks: 1.5 ticks */
#define RCU_T_MIN_SECTIONS  100u
#define RCU_T_WAIT_TICKS    3000u                    /* reader start/stop bound */

struct rcu_t_node {
    uint64_t magic;
    uint64_t gen;
    uint64_t pad[6];        /* 64 bytes: one slab class, so slots are reused */
};

static struct rcu_t_node *g_slot;
static volatile uint32_t  g_t_stop;
static volatile uint32_t  g_t_live;
static uint64_t           g_t_hold_tsc;
static uint64_t           g_t_bad;
static uint64_t           g_t_sections[PERCPU_MAX];

static inline uint64_t rcu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");   /* loads stay on their side */
    return ((uint64_t)hi << 32) | lo;
}

static void rcu_t_reader(void *arg) {
    uint32_t c = (uint32_t)(uintptr_t)arg;
    __atomic_fetch_add(&g_t_live, 1u, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&g_t_stop, __ATOMIC_ACQUIRE)) {
        uint64_t fl = rcu_read_lock();
        struct rcu_t_node *p = rcu_dereference(g_slot);
        uint64_t m0 = __atomic_load_n(&p->magic, __ATOMIC_RELAXED);
        uint64_t g0 = __atomic_load_n(&p->gen, __ATOMIC_RELAXED);
        uint64_t end = rcu_rdtsc() + g_t_hold_tsc;
        while (rcu_rdtsc() < end)
            __asm__ volatile("pause" ::: "memory");
        uint64_t m1 = __atomic_load_n(&p->magic, __ATOMIC_RELAXED);
        uint64_t g1 = __atomic_load_n(&p->gen, __ATOMIC_RELAXED);
        rcu_read_unlock(fl);
        if (m0 != RCU_T_MAGIC || m1 != RCU_T_MAGIC || g1 != g0)
            __atomic_fetch_add(&g_t_bad, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_t_sections[c], 1, __ATOMIC_RELAXED);
        yield();
    }
    __atomic_fetch_sub(&g_t_live, 1u, __ATOMIC_ACQ_REL);
    sched_exit(0);
}

static struct rcu_t_node *rcu_t_node_new(uint64_t gen) {
    struct rcu_t_node *n = kmalloc(sizeof *n);
    if (!n)
        return 0;
    n->magic = RCU_T_MAGIC;
    n->gen = gen;
    for (unsigned i = 0; i < 6; i++)
        n->pad[i] = 0;
    return n;
}

void rcu_torture(uint64_t swaps, struct rcu_torture_result *out) {
    out->cpus = 0; out->seen_cpus = 0; out->swaps = 0; out->poison_seen = 0;
    out->sections_min = 0; out->stalls = 0; out->fails = 0; out->aborted = 0;
    uint64_t stalls0 = rcu_stalls();

    /* Hold time: half a tick of TSC, measured over 20 ticks with IF set. */
    uint64_t t0 = g_ticks;
    while (g_ticks == t0)
        yield();
    uint64_t tsc0 = rcu_rdtsc(), k0 = g_ticks;
    while (g_ticks - k0 < 20)
        yield();
    uint64_t half = (rcu_rdtsc() - tsc0) / 40;
    g_t_hold_tsc = half * RCU_T_HOLD_TICKS;

    g_slot = rcu_t_node_new(1);
    if (!g_slot) {
        out->aborted = 1;
        return;
    }
    __atomic_store_n(&g_t_stop, 0u, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < PERCPU_MAX; i++) {
        struct percpu *p = percpu_get(i);
        g_t_sections[i] = 0;
        if (!p || !p->present || !__atomic_load_n(&p->shoot_ready, __ATOMIC_ACQUIRE))
            continue;
        struct tcb *r = sched_create_blocked(rcu_t_reader, (void *)(uintptr_t)i, "rcurd");
        if (!r) {
            out->aborted = 1;
            continue;
        }
        r->pin_cpu = (int32_t)i;
        sched_unblock(r);
        out->cpus++;
    }
    uint64_t dl = g_ticks + RCU_T_WAIT_TICKS;
    while (__atomic_load_n(&g_t_live, __ATOMIC_ACQUIRE) < out->cpus && g_ticks < dl)
        yield();
    if (__atomic_load_n(&g_t_live, __ATOMIC_ACQUIRE) < out->cpus)
        out->aborted = 1;

    /* The writer (RA): publish, wait, poison, free. */
    for (uint64_t s = 0; s < swaps; s++) {
        struct rcu_t_node *n = rcu_t_node_new(s + 2);
        if (!n) {
            out->aborted = 1;
            break;
        }
        struct rcu_t_node *old = g_slot;
        rcu_assign_pointer(g_slot, n);
        synchronize_rcu();
        old->magic = RCU_T_POISON;
        old->gen = RCU_T_POISON;
        for (unsigned i = 0; i < 6; i++)
            old->pad[i] = RCU_T_POISON;
        kfree(old);
        out->swaps++;
    }

    __atomic_store_n(&g_t_stop, 1u, __ATOMIC_RELEASE);
    dl = g_ticks + RCU_T_WAIT_TICKS;
    while (__atomic_load_n(&g_t_live, __ATOMIC_ACQUIRE) != 0 && g_ticks < dl)
        yield();
    if (__atomic_load_n(&g_t_live, __ATOMIC_ACQUIRE) != 0)
        out->aborted = 1;      /* a reader is still running: do not free g_slot */
    else
        kfree(g_slot);

    uint64_t mn = ~0ull;
    for (uint32_t i = 0; i < PERCPU_MAX; i++) {
        struct percpu *p = percpu_get(i);
        if (!p || !p->present || !__atomic_load_n(&p->shoot_ready, __ATOMIC_ACQUIRE))
            continue;
        uint64_t n = __atomic_load_n(&g_t_sections[i], __ATOMIC_RELAXED);
        if (n >= RCU_T_MIN_SECTIONS)
            out->seen_cpus++;
        if (n < mn)
            mn = n;
    }
    out->sections_min = (mn == ~0ull) ? 0 : mn;
    out->poison_seen = __atomic_load_n(&g_t_bad, __ATOMIC_RELAXED);
    out->stalls = rcu_stalls() - stalls0;
    out->fails = rcu_fail_bits();
}
/* ---- P5b: arms RD, RE, RF (DDR-1200 §7, Amendment 3) ----------------------
 * Every wait lock below is in .bss: a timed wait's lock must have static
 * storage (Amendment 2 point 4), and the test obeys the rule it checks. */
#define RCU_T_NW        112u
#define RCU_T_SETTLE    600u          /* ticks a sub-arm may take to finish */

struct rcu_w {
    spinlock_t        lk;
    volatile int      done;
    volatile uint32_t phase;          /* (c): 1 first wait, 2 second wait   */
    uint64_t          timeout;        /* ticks, or 0 with abs set           */
    uint64_t          abs;            /* absolute deadline in g_ticks       */
    uint64_t          timeout2;       /* (c): the second wait               */
    uint64_t          until;          /* (e): churn until this tick         */
    int32_t           pin;
    struct tcb       *self;
    volatile uint32_t armed, finished;
    int               rc, rc2;
    uint64_t          late, late_tsc, dl1;
    uint32_t          early, early2;
    uint64_t          late2, waits;
};
static struct rcu_w g_w[RCU_T_NW];
static uint64_t     g_tick_tsc;          /* TSC cycles per tick */
static volatile uint32_t g_yield_stop;

static void rcu_w_reset(struct rcu_w *w) {
    w->lk = (spinlock_t)SPINLOCK_INIT;
    w->done = 0; w->phase = 0;
    w->timeout = 0; w->abs = 0; w->timeout2 = 0; w->until = 0;
    w->pin = -1; w->self = 0;
    w->armed = 0; w->finished = 0;
    w->rc = 0; w->rc2 = 0;
    w->late = 0; w->late_tsc = 0; w->dl1 = 0;
    w->early = 0; w->early2 = 0; w->late2 = 0; w->waits = 0;
}

/* One timed wait under w->lk. Returns the result; *late is ticks past the
 * deadline when the waiter runs again, *early 1 if it timed out before it. */
static int rcu_w_wait(struct rcu_w *w, uint64_t to, uint64_t *late,
                      uint64_t *late_tsc, uint32_t *early, uint64_t *dl_out) {
    uint64_t fl = spin_lock_irqsave(&w->lk);
    uint64_t now = g_ticks;
    uint64_t dl = now + to;
    if (dl_out)
        *dl_out = dl;
    __atomic_store_n(&w->armed, 1u, __ATOMIC_RELEASE);
    uint64_t c0 = rcu_rdtsc();
    int rc = sched_block_timeout(&w->lk, &w->done, to);
    uint64_t t1 = g_ticks, c1 = rcu_rdtsc();
    spin_unlock_irqrestore(&w->lk, fl);
    if (late)
        *late = (t1 > dl) ? t1 - dl : 0;
    if (late_tsc) {
        uint64_t want = to * g_tick_tsc;
        *late_tsc = (c1 - c0 > want) ? c1 - c0 - want : 0;
    }
    if (early)
        *early = (rc == -ETIMEDOUT && t1 < dl) ? 1u : 0u;
    return rc;
}

static void rcu_t_waiter(void *arg) {
    struct rcu_w *w = arg;
    uint64_t to = w->timeout;
    if (w->abs) {
        uint64_t now = g_ticks;
        to = (w->abs > now) ? w->abs - now : 0;
    }
    w->rc = rcu_w_wait(w, to, &w->late, &w->late_tsc, &w->early, 0);
    __atomic_store_n(&w->finished, 1u, __ATOMIC_RELEASE);
    sched_exit(0);
}

/* (c): a first wait that a normal waker ends at its deadline tick, racing the
 * sweep, then at once a second wait that must time out on its own deadline. */
static void rcu_t_rewaiter(void *arg) {
    struct rcu_w *w = arg;
    w->self = current_thread;
    __atomic_store_n(&w->phase, 1u, __ATOMIC_RELEASE);
    w->rc = rcu_w_wait(w, w->timeout, 0, 0, 0, &w->dl1);
    uint64_t fl = spin_lock_irqsave(&w->lk);
    w->done = 0;
    __atomic_store_n(&w->phase, 2u, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&w->lk, fl);
    w->rc2 = rcu_w_wait(w, w->timeout2, &w->late2, 0, &w->early2, 0);
    __atomic_store_n(&w->finished, 1u, __ATOMIC_RELEASE);
    sched_exit(0);
}

/* (e): loop on 1-tick timed waits until w->until. */
static void rcu_t_churner(void *arg) {
    struct rcu_w *w = arg;
    __atomic_store_n(&w->armed, 1u, __ATOMIC_RELEASE);
    while (g_ticks < w->until) {
        rcu_w_wait(w, 1, 0, 0, 0, 0);
        w->waits++;
    }
    __atomic_store_n(&w->finished, 1u, __ATOMIC_RELEASE);
    sched_exit(0);
}

/* (f): yield in a loop on the target CPU, so its runqueue lock stays busy. */
static void rcu_t_yielder(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&g_yield_stop, __ATOMIC_ACQUIRE))
        yield();
    __atomic_fetch_sub(&g_t_live, 1u, __ATOMIC_ACQ_REL);
    sched_exit(0);
}

static uint32_t g_spawn_fail;            /* sched_create_blocked returned 0 */
static int rcu_t_spawn(thread_fn fn, void *arg, const char *nm, int32_t pin) {
    struct tcb *t = sched_create_blocked(fn, arg, nm);
    if (!t) {
        g_spawn_fail++;
        return 0;
    }
    t->pin_cpu = pin;
    sched_unblock(t);
    return 1;
}

/* Wait until n waiters from first have finished, or a bound. 1 = all did. */
static int rcu_w_join(uint32_t first, uint32_t n, uint64_t bound) {
    uint64_t dl = g_ticks + bound;
    for (;;) {
        uint32_t f = 0;
        for (uint32_t i = first; i < first + n; i++)
            f += __atomic_load_n(&g_w[i].finished, __ATOMIC_ACQUIRE);
        if (f == n)
            return 1;
        if (g_ticks >= dl)
            return 0;
        yield();
    }
}
static uint32_t rcu_w_armed(uint32_t first, uint32_t n) {
    uint32_t a = 0;
    for (uint32_t i = first; i < first + n; i++)
        a += __atomic_load_n(&g_w[i].armed, __ATOMIC_ACQUIRE);
    return a;
}
static inline void rcu_umax32(uint32_t *m, uint64_t v) { if (v > *m) *m = (uint32_t)v; }
static inline void rcu_umax64(uint64_t *m, uint64_t v) { if (v > *m) *m = v; }

/* The highest ticking CPU, for the pinned sub-arms; -1 if only the BSP ticks. */
static int32_t rcu_t_last_cpu(void) {
    int32_t last = -1;
    for (uint32_t i = 1; i < PERCPU_MAX; i++) {
        struct percpu *p = percpu_get(i);
        if (p && p->present && __atomic_load_n(&p->shoot_ready, __ATOMIC_ACQUIRE))
            last = (int32_t)i;
    }
    return last;
}

/* The RF victim: exit at once. Its tcb carries g_rcu_rf_name, so the sweep
 * pauses on it (phase 1) and schedule_locked holds it on-CPU (phase 2). */
static void rcu_t_victim(void *arg) {
    (void)arg;
    sched_exit(0);
}

/* Wait until this test's exited threads have been reaped and their memory
 * freed: the ring is back to its length at the start, and no tcb is still
 * waiting for its grace period. The kernel holds about 128 threads at most
 * (each takes two of kheap's 256 large-allocation slots: a 10,240-byte tcb
 * and a 16 KiB stack), so a sub-arm must not start on top of the last one's
 * zombies. Records the longest wait; 0 if it never settled. */
static int rcu_t_settle(struct rcu_p5b_result *out) {
    uint64_t t0 = g_ticks;
    while (__atomic_load_n(&g_rcu_sw.ring_len_last, __ATOMIC_RELAXED) > out->ring_base + 2u ||
           __atomic_load_n(&g_rcu_sw.deferred_live, __ATOMIC_ACQUIRE) != 0) {
        if (g_ticks - t0 >= 3000u) {
            out->aborted = 9;
            return 0;
        }
        yield();
    }
    rcu_umax32(&out->settle_max, g_ticks - t0);
    return 1;
}

void rcu_torture_p5b(struct rcu_p5b_result *out) {
    for (uint32_t i = 0; i < sizeof *out; i++)
        ((uint8_t *)out)[i] = 0;

    /* TSC per tick, over 20 ticks with IF set. */
    uint64_t k = g_ticks;
    while (g_ticks == k)
        yield();
    uint64_t tsc0 = rcu_rdtsc(), k0 = g_ticks;
    while (g_ticks - k0 < 20)
        yield();
    g_tick_tsc = (rcu_rdtsc() - tsc0) / 20;
    out->ring_base = (uint32_t)__atomic_load_n(&g_rcu_sw.ring_len_last, __ATOMIC_RELAXED);

    /* (a) one timed wait at a time, 20 times. */
    for (uint32_t r = 0; r < 20; r++) {
        rcu_w_reset(&g_w[0]);
        g_w[0].timeout = 50;
        if (!rcu_t_spawn(rcu_t_waiter, &g_w[0], "rcuwa", -1) ||
            !rcu_w_join(0, 1, RCU_T_SETTLE)) {
            out->aborted = 1;
            return;
        }
        if (g_w[0].rc == -ETIMEDOUT && !g_w[0].early)
            out->woke++;
        rcu_umax32(&out->late_max, g_w[0].late);
        rcu_umax64(&out->late_tsc_max, g_w[0].late_tsc);
    }

    if (!rcu_t_settle(out))
        return;
    /* (b) saturation: 80 waiters on one deadline. acts_max is reset here, so
     * sat_acts_max is the most timeouts any single sweep acted on in (b). */
    {
        const uint32_t n = 80;
        uint64_t abs = g_ticks + 60;
        for (uint32_t i = 0; i < n; i++) {
            rcu_w_reset(&g_w[i]);
            g_w[i].abs = abs;
        }
        __atomic_store_n(&g_rcu_sw.acts_max, 0, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < n; i++)
            if (!rcu_t_spawn(rcu_t_waiter, &g_w[i], "rcuwb", -1))
                out->aborted = 2;
        while (g_ticks + 5 < abs && rcu_w_armed(0, n) < n)
            yield();
        out->sat_armed = rcu_w_armed(0, n);
        if (!rcu_w_join(0, n, RCU_T_SETTLE)) {
            out->aborted = 2;
            return;
        }
        out->sat_acts_max = __atomic_load_n(&g_rcu_sw.acts_max, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < n; i++) {
            if (g_w[i].rc == -ETIMEDOUT && !g_w[i].early)
                out->sat_woke++;
            rcu_umax32(&out->sat_late_max, g_w[i].late);
            rcu_umax64(&out->late_tsc_max, g_w[i].late_tsc);
        }
    }

    if (!rcu_t_settle(out))
        return;
    /* (c) re-wait: the waker ends the first wait at its deadline tick, racing
     * the sweep; the second wait must time out on its own deadline. */
    for (uint32_t r = 0; r < 20; r++) {
        struct rcu_w *w = &g_w[0];
        rcu_w_reset(w);
        w->timeout = 10;
        w->timeout2 = 10;
        if (!rcu_t_spawn(rcu_t_rewaiter, w, "rcuwc", -1)) {
            out->aborted = 3;
            return;
        }
        uint64_t dl = g_ticks + RCU_T_SETTLE;
        while (!__atomic_load_n(&w->armed, __ATOMIC_ACQUIRE) && g_ticks < dl)
            yield();
        uint64_t d1 = w->dl1;
        while (g_ticks < d1 && g_ticks < dl)
            __asm__ volatile("pause" ::: "memory");
        uint64_t fl = spin_lock_irqsave(&w->lk);
        if (__atomic_load_n(&w->phase, __ATOMIC_ACQUIRE) == 1u && w->self) {
            w->done = 1;
            sched_unblock(w->self);
        }
        spin_unlock_irqrestore(&w->lk, fl);
        if (!rcu_w_join(0, 1, RCU_T_SETTLE)) {
            out->aborted = 3;
            return;
        }
        if (w->rc2 == -ETIMEDOUT && !w->early2 && w->late2 <= 1)
            out->rewait_ok++;
        out->rewait_early += w->early2;
    }

    if (!rcu_t_settle(out))
        return;
    /* (d) head-of-line: 32 expired waiters whose locks a pinned holder keeps
     * for 20 ticks, and one more on a free lock, all on one deadline. */
    {
        const uint32_t n = 33;
        uint64_t abs = g_ticks + 40;
        for (uint32_t i = 0; i < n; i++) {
            rcu_w_reset(&g_w[i]);
            g_w[i].abs = abs;
        }
        for (uint32_t i = 0; i < n; i++)
            if (!rcu_t_spawn(rcu_t_waiter, &g_w[i], "rcuwd", -1))
                out->aborted = 4;
        while (g_ticks + 3 < abs)
            yield();
        /* This thread holds the 32 locks with IF clear for 20 ticks of g_ticks.
         * It must not be the BSP (whose tick advances g_ticks): pin it away. */
        int32_t hc = rcu_t_last_cpu();
        if (hc > 0) {
            current_thread->pin_cpu = hc;
            yield();
        }
        uint64_t fl;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
        for (uint32_t i = 0; i < 32; i++)
            spin_lock(&g_w[i].lk);
        uint64_t rel = abs + 20;
        uint64_t spin_dl = rcu_rdtsc() + 200 * g_tick_tsc;
        while (g_ticks < rel && rcu_rdtsc() < spin_dl)
            __asm__ volatile("pause" ::: "memory");
        for (uint32_t i = 0; i < 32; i++)
            spin_unlock(&g_w[i].lk);
        __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
        current_thread->pin_cpu = -1;
        if (!rcu_w_join(0, n, RCU_T_SETTLE)) {
            out->aborted = 4;
            return;
        }
        if (g_w[32].rc == -ETIMEDOUT && !g_w[32].early)
            out->hol_woke = 1;
        out->hol_late = (uint32_t)g_w[32].late;
        rcu_umax64(&out->late_tsc_max, g_w[32].late_tsc);
    }

    if (!rcu_t_settle(out))
        return;
    /* (e) re-block churn: 64 threads on 1-tick waits while one waits 50. */
    {
        const uint32_t n = 64;
        uint64_t until = g_ticks + 120;
        for (uint32_t i = 1; i <= n; i++) {
            rcu_w_reset(&g_w[i]);
            g_w[i].until = until;
            if (!rcu_t_spawn(rcu_t_churner, &g_w[i], "rcuwe", -1))
                out->aborted = 5;
        }
        while (rcu_w_armed(1, n) < n && g_ticks < until)
            yield();
        rcu_w_reset(&g_w[0]);
        g_w[0].timeout = 50;
        if (!rcu_t_spawn(rcu_t_waiter, &g_w[0], "rcuwe", -1) ||
            !rcu_w_join(0, n + 1, RCU_T_SETTLE)) {
            out->aborted = 5;
            return;
        }
        if (g_w[0].rc == -ETIMEDOUT && !g_w[0].early)
            out->churn_woke = 1;
        out->churn_late = (uint32_t)g_w[0].late;
        rcu_umax64(&out->late_tsc_max, g_w[0].late_tsc);
        for (uint32_t i = 1; i <= n; i++)
            out->churn_waits += (uint32_t)g_w[i].waits;
    }

    if (!rcu_t_settle(out))
        return;
    /* (f) runqueue contention: 64 waiters pinned to one CPU expire together
     * while three threads pinned to that CPU yield in a loop. */
    {
        const uint32_t n = 64;
        int32_t tc = rcu_t_last_cpu();
        uint64_t c0 = __atomic_load_n(&g_rcu_sw.rq_contended, __ATOMIC_RELAXED);
        __atomic_store_n(&g_yield_stop, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_t_live, 0u, __ATOMIC_RELEASE);
        for (uint32_t y = 0; y < 3; y++) {
            __atomic_fetch_add(&g_t_live, 1u, __ATOMIC_ACQ_REL);
            if (!rcu_t_spawn(rcu_t_yielder, 0, "rcuyl", tc)) {
                __atomic_fetch_sub(&g_t_live, 1u, __ATOMIC_ACQ_REL);
                out->aborted = 6;
            }
        }
        uint64_t abs = g_ticks + 60;
        for (uint32_t i = 0; i < n; i++) {
            rcu_w_reset(&g_w[i]);
            g_w[i].abs = abs;
            if (!rcu_t_spawn(rcu_t_waiter, &g_w[i], "rcuwf", tc))
                out->aborted = 6;
        }
        int joined = rcu_w_join(0, n, RCU_T_SETTLE);
        __atomic_store_n(&g_yield_stop, 1u, __ATOMIC_RELEASE);
        uint64_t dl = g_ticks + RCU_T_SETTLE;
        while (__atomic_load_n(&g_t_live, __ATOMIC_ACQUIRE) != 0 && g_ticks < dl)
            yield();
        out->f_armed = rcu_w_armed(0, n);
        for (uint32_t i = 0; i < n; i++)
            out->f_fin += __atomic_load_n(&g_w[i].finished, __ATOMIC_ACQUIRE);
        out->f_live = __atomic_load_n(&g_t_live, __ATOMIC_ACQUIRE);
        if (!joined || out->f_live != 0) {
            out->aborted = 6;
            return;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (g_w[i].rc == -ETIMEDOUT && !g_w[i].early)
                out->rq_woke++;
            rcu_umax32(&out->rq_late, g_w[i].late);
            rcu_umax64(&out->late_tsc_max, g_w[i].late_tsc);
        }
        out->rq_contended = __atomic_load_n(&g_rcu_sw.rq_contended, __ATOMIC_RELAXED) - c0;
    }

    if (!rcu_t_settle(out))
        return;
    /* RF phase 1 (U6): 500 create/exit/reap rounds; every sweep pauses about
     * a third of a tick on each victim node and then checks it is unchanged. */
    g_rcu_rf_pause_tsc = g_tick_tsc / 3;
    __atomic_store_n(&g_rcu_rf_on, 1u, __ATOMIC_RELEASE);
    for (uint32_t r = 0; r < 500; r++) {
        uint64_t u0 = __atomic_load_n(&g_rcu_sw.rf_unlinked, __ATOMIC_ACQUIRE);
        if (!sched_create(rcu_t_victim, 0, g_rcu_rf_name)) {
            out->aborted = 7;
            break;
        }
        uint64_t dl = g_ticks + RCU_T_SETTLE;
        while (__atomic_load_n(&g_rcu_sw.rf_unlinked, __ATOMIC_ACQUIRE) == u0 && g_ticks < dl)
            yield();
        if (__atomic_load_n(&g_rcu_sw.rf_unlinked, __ATOMIC_ACQUIRE) == u0) {
            out->aborted = 7;
            break;
        }
        out->rounds++;
    }
    __atomic_store_n(&g_rcu_rf_on, 0u, __ATOMIC_RELEASE);

    /* RF phase 2 (U9 window): 20 rounds, each victim held on its CPU as an
     * unlinked current ZOMBIE for two ticks. */
    __atomic_store_n(&g_rcu_rf_hold, 1u, __ATOMIC_RELEASE);
    for (uint32_t r = 0; r < 20 && !out->aborted; r++) {
        uint64_t u0 = __atomic_load_n(&g_rcu_sw.rf_unlinked, __ATOMIC_ACQUIRE);
        if (!sched_create(rcu_t_victim, 0, g_rcu_rf_name)) {
            out->aborted = 8;
            break;
        }
        uint64_t dl = g_ticks + RCU_T_SETTLE;
        while (__atomic_load_n(&g_rcu_sw.rf_unlinked, __ATOMIC_ACQUIRE) == u0 && g_ticks < dl)
            yield();
        if (__atomic_load_n(&g_rcu_sw.rf_unlinked, __ATOMIC_ACQUIRE) == u0) {
            out->aborted = 8;
            break;
        }
        /* Let its hold end before the next one starts. */
        uint64_t t = g_ticks;
        while (g_ticks - t < 4)
            yield();
        out->hold_rounds++;
    }
    __atomic_store_n(&g_rcu_rf_hold, 0u, __ATOMIC_RELEASE);
}
uint32_t rcu_torture_spawn_fail(void) { return g_spawn_fail; }
#endif
