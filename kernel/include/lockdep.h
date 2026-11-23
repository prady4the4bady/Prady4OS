/* kernel/include/lockdep.h — DDR-1209: lock-order checking (C1.4).
 *
 * Built only with LOCKDEP=1 (`make smoke-lockdep`). In the shipped kernel every
 * macro here compiles to nothing and no type changes size (DDR-1209 D1, D2). */
#pragma once
#include <stdint.h>
#include "spinlock.h"

#define LD_DEPTH 16                  /* held locks per thread (D3) */

struct ld_ent {
    void     *lock;
    uint32_t  site;                  /* low 32 bits of the acquire's call site */
    uint8_t   cls;                   /* 0: untracked */
    uint8_t   pad[3];
};

/* D3: one per thread (in struct tcb) and one per CPU for code that runs with
 * no current thread. `lost` counts pushes refused for depth, so their releases
 * are not reported as unmatched. */
struct ld_held {
    uint32_t      depth;
    uint32_t      lost;
    struct ld_ent e[LD_DEPTH];
};

#if LOCKDEP
void lockdep_class_init(spinlock_t *l, uint8_t *key, const char *name);
/* D2: every lock initialised at this call site gets one class. */
#define LOCKDEP_CLASS(lk, name) \
    do { static uint8_t ld_key_; lockdep_class_init((lk), &ld_key_, (name)); } while (0)
void lockdep_init(void);             /* kmain, once the scheduler is up */
void lockdep_selftest(void);         /* arms LB-LE, then starts ldsum */
spinlock_t *kheap_lockdep_lock(void);
spinlock_t *pmm_lockdep_lock(void);
#else
#define LOCKDEP_CLASS(lk, name) do { (void)(lk); } while (0)
#endif
