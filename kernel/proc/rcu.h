/* kernel/proc/rcu.h — kernel RCU, one grace-period scheme (DDR-1200, C1.4 P5a).
 *
 * Read side: a region with interrupts masked on this CPU (D1, the "RCU-sched"
 * shape). A reader may use what it found with rcu_dereference until the region
 * ends. Rules (§3), each checked at run time and reported as "RCU FAIL: ...":
 *   1. a read section must not block, sleep or re-enable interrupts;
 *   2. synchronize_rcu must be called with IF set, outside every read section,
 *      and holding no spinlock (the spinlock part is not checked: the tree has
 *      no per-CPU lock count to check it against);
 *   3. a CPU that is not ticking (shoot_ready == 0) runs no readers.
 *
 * Grace period (D2): every ticking CPU but the caller's has taken a timer
 * interrupt since the writer's unlink was visible. A CPU that takes an
 * interrupt has IF set at that instant, so it is outside every read section.
 *
 * Consumer (P5b, DDR-1200 §5-§6): the timer-tick timed-wake sweep walks the
 * thread ring under rcu_read_lock, and every tcb that was ever on the ring is
 * freed only after a grace period. QEMU only, not verified on real hardware. */
#pragma once
#include <stdint.h>

/* Enter / leave a read section. The value returned by rcu_read_lock is the
 * caller's RFLAGS; pass it back to rcu_read_unlock. Sections nest. */
uint64_t rcu_read_lock(void);
void     rcu_read_unlock(uint64_t fl);

/* Wait for a grace period. Blocking (it yields); kernel threads only. */
void     synchronize_rcu(void);

/* Rule 1, called by schedule(): a switch inside a read section is a defect. */
void     rcu_note_switch(void);

/* This CPU's read-section nesting depth (0 outside every section). */
uint32_t rcu_nesting(void);

/* Grace periods that printed a stall line (§3, "Stall"). */
uint64_t rcu_stalls(void);

/* Rule checks that have failed: one bit per kind of "RCU FAIL" line (0 = none). */
uint32_t rcu_fail_bits(void);

/* Print "RCU FAIL: <what> cpu=<n>" once per bit, and set that bit. Bits in use:
 * 1 unlock without lock, 2 switch inside a section, 4 synchronize_rcu misuse,
 * 8 a timed wait on a lock outside .data/.bss (DDR-1200 Amendment 2 point 4). */
void     rcu_fail_once(uint32_t bit, const char *what);

/* A load that may be used inside a read section, and a store that publishes a
 * node to lockless readers (D3). On x86 these are plain moves plus a compiler
 * barrier; the C11 orderings make the intent and the data race defined. */
#define rcu_dereference(p)       __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define rcu_assign_pointer(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* Ticks of the writer's own CPU after which a grace period that has not ended
 * prints "[rcu] stall cpu=<n> waited=<ticks>" once, and keeps waiting (§3). */
#define RCU_STALL_TICKS 1000u

#if RCU_TORTURE
/* `make smoke-rcu` only (DDR-1200 §7, arms RA, RB, RC). */
struct rcu_torture_result {
    uint32_t cpus;          /* ticking CPUs, one pinned reader each            */
    uint32_t seen_cpus;     /* readers that completed >= RCU_T_MIN_SECTIONS    */
    uint64_t swaps;         /* writer swaps, each followed by synchronize_rcu  */
    uint64_t poison_seen;   /* reads of a node that changed under a reader     */
    uint64_t sections_min;  /* fewest sections any reader completed            */
    uint64_t stalls;        /* stall lines printed during the run              */
    uint32_t fails;         /* rcu_fail_bits() at the end of the run           */
    uint32_t aborted;       /* 1: the readers could not be started or stopped  */
};
void rcu_torture(uint64_t swaps, struct rcu_torture_result *out);

/* P5b (DDR-1200 §7, arms RD, RE, RF). The sweep and the free path keep these
 * counters in the torture build only (sched.c). */
struct tcb;
struct rcu_sweep_stats {
    uint64_t sweeps;        /* RD: sweeps run, every ticking CPU                 */
    uint64_t tick_lock;     /* RD: g_sched_lock taken while a sweep ran          */
    uint64_t acts_max;      /* RE (b): most timeouts one sweep acted on          */
    uint64_t rq_contended;  /* RE (f): act pushes that found the rq lock busy    */
    uint64_t sweep_max_tsc; /* RE: longest sweep, TSC cycles (data)              */
    uint64_t ring_len_max;  /* RF: longest walk, nodes (data)                    */
    uint64_t ring_len_last; /* the last walk's length: threads on the ring now   */
    uint64_t bad_node;      /* RF: a node changed or poisoned under the sweep    */
    uint64_t deferred_max;  /* RF: most tcbs waiting for a grace period at once  */
    uint64_t deferred_live; /* tcbs pushed to rcu_defer_free and not yet freed   */
    uint64_t rf_unlinked;   /* RF: victims unlinked so far                       */
    uint64_t rf_zhits;      /* RF (U9 window): ticks on an unlinked current zombie */
};
extern struct rcu_sweep_stats g_rcu_sw;
extern const char g_rcu_rf_name[];       /* the name every RF victim carries      */
extern volatile uint32_t g_rcu_rf_on;    /* 1: the sweep pauses on RF victims     */
extern volatile uint32_t g_rcu_rf_hold;  /* 1: an exiting RF victim is held on-CPU */
extern uint64_t g_rcu_rf_pause_tsc;      /* that pause, TSC cycles                */

struct rcu_p5b_result {
    /* RE */
    uint32_t woke, late_max;                          /* (a) */
    uint32_t sat_armed, sat_woke, sat_late_max;       /* (b) */
    uint64_t sat_acts_max;                            /* (b): most acts in one sweep */
    uint32_t rewait_ok, rewait_early;                 /* (c) */
    uint32_t hol_woke, hol_late;                      /* (d) */
    uint32_t churn_woke, churn_late, churn_waits;     /* (e) */
    uint32_t rq_woke, rq_late;                        /* (f) */
    uint32_t f_armed, f_fin, f_live;                  /* (f): where it stopped */
    uint64_t rq_contended;                            /* (f) */
    uint64_t late_tsc_max;
    /* RF */
    uint32_t rounds, hold_rounds;
    uint32_t ring_base;     /* ring length when P5b started                     */
    uint32_t settle_max;    /* longest wait, ticks, for the ring to get back to
                             * ring_base after a sub-arm (zombies reaped)       */
    uint32_t aborted;       /* 0, or the step that gave up: 1-6 RE (a)-(f), 7-8 RF,
                             * 9 the ring did not settle                        */
};
void rcu_torture_p5b(struct rcu_p5b_result *out);
uint32_t rcu_torture_spawn_fail(void);
#endif
