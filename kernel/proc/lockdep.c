/* kernel/proc/lockdep.c — DDR-1209: lock-order checking for kernel spinlocks.
 *
 * LOCKDEP=1 only (`make smoke-lockdep`); in the shipped kernel this file
 * compiles to nothing and spin_lock is the DDR-1047 fast path, unchanged.
 *
 *   - a class per lock (D2): annotated (LOCKDEP_CLASS), else one per static
 *     address, else untracked and reported;
 *   - the held locks of each thread (D3), so a thread preempted while holding a
 *     plain spin_lock does not lend its locks to the next thread on the CPU;
 *   - one global graph, transitively closed (D4): an acquire of c while h is
 *     held adds h -> c, and is an inversion if c already reaches h;
 *   - the hooks never print (D6): reports go to a bounded record that the
 *     `ldsum` thread prints every 2 s.
 *
 * The checker's own lock (g_ld_lk) is a raw test-and-set taken with interrupts
 * off and never held across anything that takes a spinlock. */
#include <stdint.h>
#include "spinlock.h"
#include "lockdep.h"
#include "lock_stat.h"
#include "percpu.h"
#include "sched.h"
#include "console.h"
#include "irq.h"

#if LOCKDEP

#define LD_MAXC  128                  /* class ids 1..127; 0 = untracked */
#define LD_REPS  32
#define LD_SUM_TICKS 200u             /* ldsum period: 2 s at 100 Hz */
#define LD_FINAL_WAKES 120u           /* the freeze: ldsum's 120th wake, about 240 s */

extern char __rodata_end[], __bss_end[];

struct ld_class {
    const void *key;                  /* the lock (static) or the site's key */
    const char *name;                 /* NULL for a static lock */
    uint64_t    site;                 /* first acquire, or the annotation */
    uint8_t     test;                 /* a self-test class (D7) */
};

enum { LDR_INV = 1, LDR_TINV, LDR_REC, LDR_NEST, LDR_UNT, LDR_UNM, LDR_OVF };

struct ld_rep {
    uint8_t     kind, a, b, cpu;
    const void *lock;
    uint64_t    take, held;           /* call sites */
};

static struct ld_class g_ldc[LD_MAXC];
static volatile uint32_t g_ld_ncls = 1;
static uint64_t g_ld_dir[LD_MAXC][2];     /* direct[a] bit b: b taken under a */
static uint64_t g_ld_reach[LD_MAXC][2];   /* reach[a] bit b: a ->+ b */
static uint64_t g_ld_bad[LD_MAXC][2];     /* inverted pairs already reported */
static uint64_t g_ld_nest_seen[2];
static volatile uint8_t g_ld_lk;
static volatile uint32_t g_ld_on;
static struct ld_held g_ld_cpu[PERCPU_MAX];

static struct ld_rep g_ld_rep[LD_REPS];
static volatile uint32_t g_ld_nrep;
static uint32_t g_ld_nrep_kind[8];

static volatile uint32_t g_ld_edges, g_ld_inv, g_ld_tinv, g_ld_rec, g_ld_nest,
                         g_ld_unm, g_ld_unt, g_ld_ovf;

/* D6, the freeze (CodeRabbit on #152). Set once, by ldsum, under g_ld_lk. Every
 * counter, record, class and edge is written under g_ld_lk and only while this
 * is 0, so the snapshot taken as it is set is the state at the end of the run,
 * whenever the runner stops the guest. */
static uint32_t g_ld_frozen;

/* ---- primitives ------------------------------------------------------- */

static inline uint64_t ld_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void ld_irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}
static inline uint64_t ld_lock(void) {
    uint64_t f = ld_irq_save();
    while (__atomic_test_and_set(&g_ld_lk, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return f;
}
static inline void ld_unlock(uint64_t f) {
    __atomic_clear(&g_ld_lk, __ATOMIC_RELEASE);
    ld_irq_restore(f);
}
static inline int ld_bit(const uint64_t r[2], unsigned b) {
    return (int)((__atomic_load_n(&r[b >> 6], __ATOMIC_RELAXED) >> (b & 63u)) & 1u);
}
static inline void ld_set(uint64_t r[2], unsigned b) {
    __atomic_store_n(&r[b >> 6], r[b >> 6] | (1ull << (b & 63u)), __ATOMIC_RELAXED);
}
static inline void ld_inc(volatile uint32_t *c) {
    __atomic_add_fetch(c, 1u, __ATOMIC_RELAXED);
}
/* D3: tested before anything reads %gs. The checker turns on after the BSP's
 * per-CPU setup, and an AP sets its GS base before its first lock, so
 * this_cpu() is never read while GS can still be 0. */
static inline int ld_on(void) {
    return (int)__atomic_load_n(&g_ld_on, __ATOMIC_ACQUIRE);
}

/* Caller holds g_ld_lk. Bounded per kind, so diagnostics cannot crowd out an
 * inversion record; the counters stay exact either way. */
static void ld_report(uint8_t kind, uint8_t a, uint8_t b, const void *lock,
                      uint64_t take, uint64_t held) {
    static const uint32_t cap[8] = { 0, 16, 8, 4, 8, 8, 8, 2 };
    uint32_t n = g_ld_nrep;
    if (n >= LD_REPS || kind >= 8 || g_ld_nrep_kind[kind] >= cap[kind])
        return;
    if (kind == LDR_UNT || kind == LDR_UNM)       /* one record per lock */
        for (uint32_t i = 0; i < n; i++)
            if (g_ld_rep[i].kind == kind && g_ld_rep[i].lock == lock)
                return;
    g_ld_nrep_kind[kind]++;
    struct ld_rep *r = &g_ld_rep[n];
    r->kind = kind;
    r->a = a;
    r->b = b;
    r->cpu = ld_on() ? (uint8_t)this_cpu()->cpu_idx : 0;   /* no %gs before on */
    r->lock = lock;
    r->take = take;
    r->held = held;
    __atomic_store_n(&g_ld_nrep, n + 1u, __ATOMIC_RELEASE);
}

/* A counted event outside the graph (recursion, a full held stack, an
 * unmatched release): counted and recorded under g_ld_lk, unless frozen. */
static void ld_count(volatile uint32_t *ctr, uint8_t kind, uint8_t a, uint8_t b,
                     const void *lock, uint64_t take, uint64_t held) {
    uint64_t f = ld_lock();
    if (!g_ld_frozen) {
        ld_inc(ctr);
        ld_report(kind, a, b, lock, take, held);
    }
    ld_unlock(f);
}

/* ---- classes (D2) ----------------------------------------------------- */

/* Caller holds g_ld_lk. */
static uint8_t ld_find_key(const void *key) {
    for (uint32_t c = 1; c < g_ld_ncls; c++)
        if (g_ldc[c].key == key)
            return (uint8_t)c;
    return 0;
}

/* Caller holds g_ld_lk. 0 = the table is full (counted as ovf), or frozen. */
static uint8_t ld_new_class(const void *key, const char *name, uint64_t site, uint8_t test) {
    if (g_ld_frozen)
        return 0;
    uint32_t c = g_ld_ncls;
    if (c >= LD_MAXC) {
        ld_inc(&g_ld_ovf);
        ld_report(LDR_OVF, 0, 0, key, site, 0);
        return 0;
    }
    g_ldc[c].key = key;
    g_ldc[c].name = name;
    g_ldc[c].site = site;
    g_ldc[c].test = test;
    __atomic_store_n(&g_ld_ncls, c + 1u, __ATOMIC_RELEASE);
    return (uint8_t)c;
}

static inline void ld_stamp(spinlock_t *l, uint8_t c) {
    __atomic_store_n(&l->chk, (uint8_t)~c, __ATOMIC_RELAXED);
    __atomic_store_n(&l->cls, c, __ATOMIC_RELAXED);
}

/* A valid class, or 0. A torn pair, or random leftover bytes, fail the check
 * byte (all but 1 pair in 256). A previous lock's valid stamp on reused memory
 * does not: that case is covered only by the initialisers, which stamp or zero
 * all three bytes. That is an audit of today's tree, not a gate (DDR-1209 D2;
 * CodeRabbit on #152). */
static inline uint8_t ld_valid(const spinlock_t *l) {
    uint8_t c = __atomic_load_n(&l->cls, __ATOMIC_RELAXED);
    uint8_t k = __atomic_load_n(&l->chk, __ATOMIC_RELAXED);
    if (c && (uint8_t)~c == k && c < __atomic_load_n(&g_ld_ncls, __ATOMIC_ACQUIRE))
        return c;
    return 0;
}

static uint8_t ld_class_of(spinlock_t *l, uint64_t site) {
    uint8_t c = ld_valid(l);
    if (c)
        return c;
    uintptr_t a = (uintptr_t)l;
    int stat = a >= (uintptr_t)__rodata_end && a < (uintptr_t)__bss_end;
    uint64_t f = ld_lock();
    if (stat) {
        c = ld_find_key(l);
        if (!c)
            c = ld_new_class(l, 0, site, 0);
        if (c)
            ld_stamp(l, c);
    } else if (!g_ld_frozen) {
        /* An unannotated heap or stack lock, or leftover kmalloc bytes. */
        ld_inc(&g_ld_unt);
        ld_report(LDR_UNT, 0, 0, l, site, 0);
    }
    ld_unlock(f);
    return c;
}

void lockdep_class_init(spinlock_t *l, uint8_t *key, const char *name) {
    uint8_t c = __atomic_load_n(key, __ATOMIC_ACQUIRE);
    if (!c) {
        uint64_t f = ld_lock();
        c = *key;
        if (!c) {
            c = ld_new_class(key, name, (uint64_t)(uintptr_t)__builtin_return_address(0), 0);
            __atomic_store_n(key, c, __ATOMIC_RELEASE);
        }
        ld_unlock(f);
    }
    if (c) {
        ld_stamp(l, c);
    } else {
        __atomic_store_n(&l->cls, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&l->chk, 0, __ATOMIC_RELAXED);
    }
}

/* ---- the graph (D4) --------------------------------------------------- */

static void ld_add_edge(uint8_t a, uint8_t b, const void *lock, uint64_t take, uint64_t held) {
    uint64_t f = ld_lock();
    if (!g_ld_frozen && !ld_bit(g_ld_dir[a], b)) {
        if (ld_bit(g_ld_reach[b], a)) {
            /* b already reaches a: a -> b closes a cycle. Not added, so the
             * graph stays acyclic; each pair is counted and reported once. */
            if (!ld_bit(g_ld_bad[a], b)) {
                ld_set(g_ld_bad[a], b);
                int test = g_ldc[a].test || g_ldc[b].test;
                ld_inc(test ? &g_ld_tinv : &g_ld_inv);
                ld_report(test ? LDR_TINV : LDR_INV, a, b, lock, take, held);
            }
        } else {
            ld_set(g_ld_dir[a], b);
            ld_inc(&g_ld_edges);
            uint64_t n0 = g_ld_reach[b][0], n1 = g_ld_reach[b][1];
            if (b < 64) n0 |= 1ull << b; else n1 |= 1ull << (b - 64);
            uint32_t nc = g_ld_ncls;
            for (uint32_t x = 1; x < nc; x++)
                if (x == a || ld_bit(g_ld_reach[x], a)) {
                    __atomic_store_n(&g_ld_reach[x][0], g_ld_reach[x][0] | n0, __ATOMIC_RELAXED);
                    __atomic_store_n(&g_ld_reach[x][1], g_ld_reach[x][1] | n1, __ATOMIC_RELAXED);
                }
        }
    }
    ld_unlock(f);
}

/* ---- held locks (D3) -------------------------------------------------- */

static inline struct ld_held *ld_cpu_held(struct percpu *p) {
    uint32_t i = p->cpu_idx;
    return &g_ld_cpu[i < PERCPU_MAX ? i : 0];
}
static inline struct ld_held *ld_cur(struct percpu *p) {
    struct tcb *t = p->current;
    return t ? &t->ld : ld_cpu_held(p);
}
static inline int ld_active(struct percpu *p) {
    (void)p;
    return 1;
}

/* Before the wait: the order check. Returns the class for the push. */
static uint8_t ld_check(spinlock_t *l, uint64_t site) {
    if (!ld_on())
        return 0;
    uint64_t f = ld_irq_save();
    struct percpu *p = this_cpu();
    uint8_t c = 0;
    if (ld_active(p)) {
        struct ld_held *h = ld_cur(p);
        c = ld_class_of(l, site);
        for (uint32_t i = 0; i < h->depth; i++) {
            struct ld_ent *e = &h->e[i];
            if (e->lock == (void *)l) {
                ld_count(&g_ld_rec, LDR_REC, c, c, l, site, e->site);
                continue;
            }
            if (!c || !e->cls)
                continue;
            if (e->cls == c) {
                uint64_t g = ld_lock();         /* counted under g_ld_lk (D6) */
                if (!g_ld_frozen) {
                    ld_inc(&g_ld_nest);
                    if (!ld_bit(g_ld_nest_seen, c)) {
                        ld_set(g_ld_nest_seen, c);
                        ld_report(LDR_NEST, c, c, l, site, e->site);
                    }
                }
                ld_unlock(g);
                continue;
            }
            if (ld_bit(g_ld_dir[e->cls], c))
                continue;                      /* known edge: one bit test */
            ld_add_edge(e->cls, c, l, site, e->site);
        }
    }
    ld_irq_restore(f);
    return c;
}

static void ld_push(spinlock_t *l, uint64_t site, uint8_t c) {
    if (!ld_on())
        return;
    uint64_t f = ld_irq_save();
    struct percpu *p = this_cpu();
    if (ld_active(p)) {
        struct ld_held *h = ld_cur(p);
        if (h->depth >= LD_DEPTH) {
            h->lost++;
            ld_count(&g_ld_ovf, LDR_OVF, c, 0, l, site, 0);
        } else {
            struct ld_ent *e = &h->e[h->depth++];
            e->lock = l;
            e->site = (uint32_t)site;
            e->cls = c;
        }
    }
    ld_irq_restore(f);
}

static int ld_take(struct ld_held *h, const spinlock_t *l) {
    for (uint32_t i = h->depth; i-- > 0; )
        if (h->e[i].lock == (const void *)l) {
            for (uint32_t j = i + 1; j < h->depth; j++)
                h->e[j - 1] = h->e[j];
            h->depth--;
            return 1;
        }
    return 0;
}

static void ld_pop(spinlock_t *l, uint64_t site) {
    if (!ld_on())
        return;
    uint64_t f = ld_irq_save();
    struct percpu *p = this_cpu();
    if (ld_active(p)) {
        struct ld_held *h = ld_cur(p);
        if (!ld_take(h, l)) {
            /* Taken with no current thread on this CPU (an AP adopting its
             * idle under a lock), released once it has one. */
            struct ld_held *ch = ld_cpu_held(p);
            if (ch == h || !ld_take(ch, l)) {
                if (h->lost) {
                    h->lost--;
                } else {
                    ld_count(&g_ld_unm, LDR_UNM, 0, 0, l, site, 0);
                }
            }
        }
    }
    ld_irq_restore(f);
}

/* ---- the spinlock entry points (spinlock.h, LOCKDEP=1) ------------------ */

void lockdep_spin_lock(spinlock_t *l) {
    uint64_t site = (uint64_t)(uintptr_t)__builtin_return_address(0);
    uint8_t c = ld_check(l, site);             /* D6: before the wait */
    if (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        spin_lock_contended(l);
    ld_push(l, site, c);
}

void lockdep_spin_unlock(spinlock_t *l) {
    ld_pop(l, (uint64_t)(uintptr_t)__builtin_return_address(0));
    __atomic_clear(&l->v, __ATOMIC_RELEASE);
}

int lockdep_spin_trylock(spinlock_t *l) {
    uint64_t site = (uint64_t)(uintptr_t)__builtin_return_address(0);
    if (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        return 0;
    /* D5: a trylock never waits, so it adds no edge into l; it is pushed so
     * later acquisitions get edges from it. */
    uint8_t c = 0;
    if (ld_on())
        c = ld_class_of(l, site);
    ld_push(l, site, c);
    return 1;
}

/* D3: called before smp_start_aps, so no AP can hold a lock across the
 * switch-on (a release with no matching acquire would read as unmatched), and
 * again after sched_init for a boot with no LAPIC. Until the scheduler is up
 * the BSP has no current thread and uses its per-CPU fallback stack. */
void lockdep_init(void) {
    if (__atomic_exchange_n(&g_ld_on, 1u, __ATOMIC_ACQ_REL))
        return;
    kputs("[lockdep] on (DDR-1209)\r\n");
}

/* ---- reporting (D6) ---------------------------------------------------- */

static void ld_cls(kline *k, uint8_t c) {
    if (!c || c >= g_ld_ncls) {
        kline_s(k, "-");
        return;
    }
    if (g_ldc[c].name)
        kline_s(k, g_ldc[c].name);
    else
        kline_x(k, (uint64_t)(uintptr_t)g_ldc[c].key);
    kline_c(k, '#');
    kline_d(k, c);
}

static void ld_print_rep(const struct ld_rep *r) {
    kline k;
    kline_init(&k);
    switch (r->kind) {
    case LDR_INV:  kline_s(&k, "LOCKDEP INVERSION held=");      break;
    case LDR_TINV: kline_s(&k, "[lockdep] test-inversion held="); break;
    case LDR_REC:  kline_s(&k, "LOCKDEP RECURSION class=");     break;
    case LDR_NEST: kline_s(&k, "[lockdep] nest class=");        break;
    case LDR_UNT:  kline_s(&k, "[lockdep] untracked lock=");    break;
    case LDR_UNM:  kline_s(&k, "[lockdep] unmatched lock=");    break;
    default:       kline_s(&k, "[lockdep] overflow lock=");     break;
    }
    if (r->kind == LDR_INV || r->kind == LDR_TINV) {
        ld_cls(&k, r->a);
        kline_s(&k, " take=");
        ld_cls(&k, r->b);
        kline_s(&k, " held_at=");
        kline_x(&k, 0xffffffff00000000ull | (uint32_t)r->held);
    } else if (r->kind == LDR_REC || r->kind == LDR_NEST) {
        ld_cls(&k, r->a);
        kline_s(&k, " lock=");
        kline_x(&k, (uint64_t)(uintptr_t)r->lock);
    } else {
        kline_x(&k, (uint64_t)(uintptr_t)r->lock);
    }
    kline_s(&k, " at=");
    kline_x(&k, r->take);
    kline_s(&k, " cpu=");
    kline_d(&k, r->cpu);
    kline_s(&k, "\r\n");
    kline_emit(&k);
}

static int ld_has_edge(spinlock_t *a, spinlock_t *b) {
    uint8_t ca = ld_valid(a), cb = ld_valid(b);
    return ca && cb && ld_bit(g_ld_dir[ca], cb);
}

static spinlock_t g_ld_sum_lk = SPINLOCK_INIT;
static volatile int g_ld_never;

static void ld_values(uint64_t v[10]) {
    v[0] = g_ld_ncls - 1u;
    v[1] = g_ld_edges;
    v[2] = g_ld_inv;
    v[3] = g_ld_rec;
    v[4] = g_ld_nest;
    v[5] = g_ld_unm;
    v[6] = g_ld_unt;
    v[7] = g_ld_ovf;
    v[8] = g_ld_tinv;
    v[9] = (uint64_t)ld_has_edge(kheap_lockdep_lock(), pmm_lockdep_lock());
}

static void ld_sum_line(const char *head, const uint64_t v[10]) {
    static const char *const key[10] = {
        "classes=", " edges=", " inv=", " rec=", " nest=",
        " unmatched=", " untracked=", " ovf=", " tinv=", " hp=" };
    kline k;
    kline_init(&k);
    kline_s(&k, head);
    for (unsigned i = 0; i < 10; i++) {
        kline_s(&k, key[i]);
        kline_d(&k, v[i]);
    }
    kline_s(&k, "\r\n");
    kline_emit(&k);
}

/* A summary whenever a value changed. At wake LD_FINAL_WAKES, the freeze: from
 * then on nothing is counted, recorded, classed or added to the graph, so the
 * "[lockdep] final" line printed then is the state at the end of the run. Arm
 * LA reads only that line, and requires it after the workload's last line. */
static void ld_sum(void *arg) {
    (void)arg;
    uint32_t shown = 0, frozen = 0;
    uint64_t last[10], v[10];
    for (unsigned i = 0; i < 10; i++)
        last[i] = ~0ull;
    for (uint32_t wake = 1; ; wake++) {
        uint64_t fl = spin_lock_irqsave(&g_ld_sum_lk);
        (void)sched_block_timeout(&g_ld_sum_lk, &g_ld_never, LD_SUM_TICKS);
        spin_unlock_irqrestore(&g_ld_sum_lk, fl);
        if (frozen)
            continue;
        uint32_t n;
        if (wake == LD_FINAL_WAKES) {
            uint64_t f = ld_lock();
            g_ld_frozen = 1;
            ld_values(v);
            n = g_ld_nrep;
            ld_unlock(f);
            frozen = 1;
        } else {
            n = __atomic_load_n(&g_ld_nrep, __ATOMIC_ACQUIRE);
            ld_values(v);
        }
        while (shown < n)
            ld_print_rep(&g_ld_rep[shown++]);
        if (frozen) {
            ld_sum_line("[lockdep] final ", v);
            continue;
        }
        int changed = 0;
        for (unsigned i = 0; i < 10; i++)
            if (v[i] != last[i]) {
                changed = 1;
                last[i] = v[i];
            }
        if (changed)
            ld_sum_line("[lockdep] sum ", v);
    }
}

/* ---- self-tests, arms LB-LE (D7) -------------------------------------- */

static spinlock_t tX = SPINLOCK_INIT, tY = SPINLOCK_INIT;
static spinlock_t tP = SPINLOCK_INIT, tQ = SPINLOCK_INIT, tR = SPINLOCK_INIT;
static spinlock_t tU = SPINLOCK_INIT, tV = SPINLOCK_INIT;
static spinlock_t tA = SPINLOCK_INIT, tB = SPINLOCK_INIT;

static void ld_test_class(spinlock_t *l) {
    uint64_t f = ld_lock();
    uint8_t c = ld_find_key(l);
    if (!c)
        c = ld_new_class(l, 0, 0, 1);
    else
        g_ldc[c].test = 1;
    if (c)
        ld_stamp(l, c);
    ld_unlock(f);
}

static void ld_pair(spinlock_t *outer, spinlock_t *inner) {
    spin_lock(outer);
    spin_lock(inner);
    spin_unlock(inner);
    spin_unlock(outer);
}

static void ld_line(const char *tag, const char *k1, uint64_t v1, const char *k2, uint64_t v2) {
    kline k;
    kline_init(&k);
    kline_s(&k, tag);
    kline_s(&k, k1);
    kline_d(&k, v1);
    if (k2) {
        kline_s(&k, k2);
        kline_d(&k, v2);
    }
    kline_s(&k, "\r\n");
    kline_emit(&k);
}

static volatile uint32_t g_t1_holds, g_t2_done, g_t1_done, g_overlap;
static volatile uint32_t g_te1_done, g_te2_done, g_te1_cpu, g_te2_cpu;

/* LD, thread 1: holds U with interrupts enabled until thread 2 has run on this
 * same CPU, i.e. until the tick has switched it out while it held U. */
static void ld_t1(void *arg) {
    (void)arg;
    __asm__ volatile("sti");
    spin_lock(&tU);
    __atomic_store_n(&g_t1_holds, 1u, __ATOMIC_RELEASE);
    uint64_t dl = g_ticks + 500;
    while (!__atomic_load_n(&g_t2_done, __ATOMIC_ACQUIRE) && g_ticks < dl)
        __asm__ volatile("pause");
    g_overlap = __atomic_load_n(&g_t2_done, __ATOMIC_ACQUIRE);
    spin_unlock(&tU);
    __atomic_store_n(&g_t1_done, 1u, __ATOMIC_RELEASE);
    sched_exit(0);
}

static void ld_t2(void *arg) {
    (void)arg;
    uint64_t dl = g_ticks + 500;
    while (!__atomic_load_n(&g_t1_holds, __ATOMIC_ACQUIRE) && g_ticks < dl)
        yield();
    spin_lock(&tV);
    spin_unlock(&tV);
    __atomic_store_n(&g_t2_done, 1u, __ATOMIC_RELEASE);
    sched_exit(0);
}

/* LE: each order on its own pinned AP. Interrupts are off around the pair,
 * so the CPU recorded is the one that took both locks. */
static void ld_te1(void *arg) {
    (void)arg;
    uint64_t f = ld_irq_save();
    ld_pair(&tA, &tB);
    g_te1_cpu = this_cpu()->cpu_idx;
    ld_irq_restore(f);
    __atomic_store_n(&g_te1_done, 1u, __ATOMIC_RELEASE);
    sched_exit(0);
}

static void ld_te2(void *arg) {
    (void)arg;
    uint64_t f = ld_irq_save();
    ld_pair(&tB, &tA);
    g_te2_cpu = this_cpu()->cpu_idx;
    ld_irq_restore(f);
    __atomic_store_n(&g_te2_done, 1u, __ATOMIC_RELEASE);
    sched_exit(0);
}

static int ld_spawn(thread_fn fn, const char *nm, int32_t pin) {
    struct tcb *t = sched_create_blocked(fn, 0, nm);
    if (!t)
        return 0;
    t->pin_cpu = pin;
    sched_unblock(t);
    return 1;
}

static int ld_wait(volatile uint32_t *flag, uint64_t bound) {
    uint64_t dl = g_ticks + bound;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE) && g_ticks < dl)
        yield();
    return (int)__atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

void lockdep_selftest(void) {
    spinlock_t *tl[] = { &tX, &tY, &tP, &tQ, &tR, &tU, &tV, &tA, &tB };
    for (unsigned i = 0; i < sizeof tl / sizeof tl[0]; i++)
        ld_test_class(tl[i]);

    /* Two APs that can run threads, as rcu_torture picks its readers. */
    int32_t ap[2] = { -1, -1 };
    for (uint32_t i = 0, n = 0; i < PERCPU_MAX && n < 2; i++) {
        struct percpu *p = percpu_get(i);
        if (p && p->present && !p->is_bsp &&
            __atomic_load_n(&p->shoot_ready, __ATOMIC_ACQUIRE))
            ap[n++] = (int32_t)i;
    }

    /* LB: a direct inversion. */
    uint32_t t0 = g_ld_tinv;
    ld_pair(&tX, &tY);
    ld_pair(&tY, &tX);
    ld_line("PRADYOS_LOCKDEP_B", " inv=", g_ld_tinv - t0, 0, 0);

    /* LC: P -> Q -> R, then R -> P; no pair is inverted directly. */
    t0 = g_ld_tinv;
    ld_pair(&tP, &tQ);
    ld_pair(&tQ, &tR);
    ld_pair(&tR, &tP);
    ld_line("PRADYOS_LOCKDEP_C", " inv=", g_ld_tinv - t0, 0, 0);

    /* LD: two threads on one AP; T2 takes V while the preempted T1 holds U.
     * Then V -> U here. Per-thread state records no U -> V edge. */
    uint32_t ok = 0;
    if (ap[0] >= 0 && ld_spawn(ld_t1, "ldt1", ap[0]) && ld_spawn(ld_t2, "ldt2", ap[0]))
        ok = ld_wait(&g_t1_done, 800) && ld_wait(&g_t2_done, 100);
    t0 = g_ld_tinv;
    ld_pair(&tV, &tU);
    ld_line("PRADYOS_LOCKDEP_D", " overlap=", ok ? g_overlap : 0u, " inv=", g_ld_tinv - t0);

    /* LE: A -> B on one AP, then B -> A on another. cross=1 says the two
     * orders really were taken on different CPUs. */
    ok = 0;
    t0 = g_ld_tinv;
    if (ap[0] >= 0 && ap[1] >= 0 && ld_spawn(ld_te1, "ldte1", ap[1]) &&
        ld_wait(&g_te1_done, 500) && ld_spawn(ld_te2, "ldte2", ap[0]))
        ok = (uint32_t)ld_wait(&g_te2_done, 500);
    ld_line("PRADYOS_LOCKDEP_E", " cross=", ok ? (uint64_t)(g_te1_cpu != g_te2_cpu) : 0u,
            " inv=", g_ld_tinv - t0);

    if (!sched_create(ld_sum, 0, "ldsum"))
        kputs("[lockdep] FAILED to start ldsum\r\n");
}

#endif /* LOCKDEP */
