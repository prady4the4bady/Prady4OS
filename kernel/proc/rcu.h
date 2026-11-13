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
 * P5a has no product consumer: until P5b (DDR-1200 Amendment 1) only the
 * RCU_TORTURE self-test uses this. QEMU only, not verified on real hardware. */
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
#endif
