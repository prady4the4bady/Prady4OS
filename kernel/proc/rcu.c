/* kernel/proc/rcu.c — kernel RCU, one grace-period scheme (DDR-1200, C1.4 P5a).
 * See rcu.h for the rules. QEMU only, not verified on real hardware. */
#include "rcu.h"
#include "percpu.h"
#include "console.h"
#include "sched.h"
#include "kheap.h"
#include "irq.h"

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

static void rcu_fail_once(uint32_t bit, const char *what) {
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

    static volatile int g_mut_u1 = 1;
    if (g_mut_u1)
        return;                 /* MUTANT U1: synchronize_rcu returns at once */
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
#endif
