/* user/pthrtest.c — pthr.h arms for smoke-thread (DDR-1197 §4, Amendment 1).
 *
 * Linked into threadtest.elf and called from its entry point (Amendment 1: a new
 * probe would need a kernel/main.c launch, which this part may not touch).
 * Compiled alone with -DPTHR_FREESTANDING and only pthr.h (plus the page
 * meter), so the header is shown to be self-contained.
 *
 *   PA create, join, return value     PR reclaim (page meter)
 *   PJ join sleeps in the kernel      PD detach and reap
 *   PG guard page faults              PK keys: isolation, destructors, stale
 *   PO once: one run, callers wait for it
 *
 * Every arm runs in a forked child that calls pthr_init itself: the parent
 * forks BEFORE any pthr_init, as the post-fork contract requires (DDR-1197
 * §6). The child reports through a MAP_SHARED page; the parent judges after a
 * bounded reap, so a mutant that leaves a thread asleep prints timeout=1.
 *
 * Freestanding: raw syscalls, no writable globals (user.ld, DDR-826).
 */
#include "pthr.h"
#include "pagemeter.h"

#define MS            1000000ull
#define VDSO_USER_VA  0x00007FFFFFF00000ull
#define WNOHANG       1
#define SIGKILL       9

#define sys pradyos_syscall

void pthr_arms(void);

static uint64_t now(void) { return *(volatile uint64_t *)VDSO_USER_VA; }

/* One line, one write (DDR-1182 §3.1); %d (long) and %s only. */
static void say(const char *fmt, ...) {
    char b[192];
    long n = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char *f = fmt; *f && n < (long)sizeof b - 21; f++) {
        if (f[0] == '%' && f[1] == 'd') {
            long v = __builtin_va_arg(ap, long);
            unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
            char d[20];
            int nd = 0;
            do { d[nd++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) b[n++] = '-';
            while (nd) b[n++] = d[--nd];
            f++;
        } else if (f[0] == '%' && f[1] == 's') {
            for (const char *s = __builtin_va_arg(ap, const char *);
                 *s && n < (long)sizeof b - 1; s++)
                b[n++] = *s;
            f++;
        } else {
            b[n++] = *f;
        }
    }
    __builtin_va_end(ap);
    sys(SYS_WRITE, 1, (long)b, n);
}

__attribute__((noreturn)) static void fail(const char *why) {
    say("PTHR FAIL: %s\n", why);
    sys(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static void yield(void) { sys(SYS_YIELD, 0, 0, 0); }

/* Spin to the next vDSO tick edge (at most one 10 ms tick). */
static void linger_tick(void) {
    uint64_t e = now();
    while (now() == e) yield();
}

/* Results shared between an arm's child (all its threads) and the parent. */
struct pt {
    volatile long placed, pidx, to;
    pthr_mutex    m;                       /* PA */
    volatile long ctr, joined, ret_ok;
    volatile long go, asleep, jret_ok, jdone;          /* PJ */
    volatile long rounds, conclusive, drop, trials;    /* PR */
    volatile long rej_pre, rej_post, rej_procs, parent;
    volatile long detached, dgo, reclaimed;            /* PD */
    volatile long survived;                            /* PG */
    pthr_barrier  bar;                                 /* PK, PO */
    volatile long iso, dtor, stale, same_slot;         /* PK */
    pthr_once_t   once;                                /* PO */
    volatile long runs, odone, waited;
};

static void pt_clear(struct pt *p) {
    volatile uint64_t *w = (volatile uint64_t *)p;
    for (unsigned long k = 0; k < sizeof *p / 8; k++) w[k] = 0;
}

/* Pin to `cpu`; the placement counts only if GET confirms it (DDR-1193 A1 item 2). */
static void place(struct pt *p, long cpu) {
    if (sys(SYS_SCHED_CPU, 1, cpu, 0) == 0 && sys(SYS_SCHED_CPU, 0, 0, 0) == cpu)
        __atomic_add_fetch(&p->placed, 1, __ATOMIC_SEQ_CST);
}

/* 1 if the child exited by itself within bound_ms, 0 if it had to be killed. */
static int reap(long pid, uint64_t bound_ms) {
    int st = 0;
    uint64_t end = now() + bound_ms * MS;
    for (;;) {
        long r = sys(SYS_WAIT4, pid, (long)&st, WNOHANG);
        if (r == pid) return 1;
        if (r != -11 /* -EAGAIN */) fail("wait4");
        if (now() > end) break;
        yield();
    }
    sys(SYS_KILL, pid, SIGKILL, 0);
    end = now() + 5000 * MS;
    while (sys(SYS_WAIT4, pid, (long)&st, WNOHANG) != pid) {
        if (now() > end) fail("killed child never reaped");
        yield();
    }
    return 0;
}

/* Run one arm body in a forked child; returns 1 on timeout (killed). */
static long run(struct pt *p, void (*body)(struct pt *), uint64_t bound_ms, long *ms) {
    pt_clear(p);
    p->parent = sys(SYS_GETPID, 0, 0, 0);
    uint64_t t0 = now();
    long c = sys(SYS_FORK, 0, 0, 0);
    if (c < 0) fail("fork");
    if (c == 0) {
        if (pthr_init() != 0) fail("init");
        body(p);
        sys(SYS_EXIT, 0, 0, 0);            /* group exit ends any stuck thread */
        for (;;) { }
    }
    long ended = reap(c, bound_ms);
    *ms = (long)((now() - t0) / MS);
    return p->to || !ended;
}

/* 1 once SYS_GETPROCS shows tid `tid` THREAD_BLOCKED (3) (DDR-1193 §4). */
static int blocked(long tid, uint64_t bound_ms) {
    struct { uint32_t pid, ppid, state, flags; char name[16];
             uint64_t run_ticks, dispatches; } pi;
    uint64_t end = now() + bound_ms * MS;
    for (;;) {
        for (long i = 0; i < 4096 && sys(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++)
            if ((long)pi.pid == tid) {
                if (pi.state == 3) return 1;
                break;
            }
        if (now() > end) return 0;
        yield();
    }
}

/* ---- PA: create, join, return value -------------------------------------- */

struct pa_arg { struct pt *p; long idx; };

static void *pa_fn(void *a) {
    struct pa_arg *x = (struct pa_arg *)a;
    struct pt *p = x->p;
    place(p, x->idx & 3);
    linger_tick();                         /* so a join that does not wait (R1) reads ret too early */
    for (int k = 0; k < 1000; k++) {
        pthr_mutex_lock(&p->m);
        p->ctr = p->ctr + 1;
        pthr_mutex_unlock(&p->m);
    }
    return (void *)(x->idx * 3);
}

static void b_pa(struct pt *p) {
    struct pa_arg args[8];
    pthr_t th[8];
    pthr_mutex_init(&p->m);
    for (long i = 0; i < 8; i++) {
        args[i].p = p;
        args[i].idx = i + 1;
        if (pthr_create(&th[i], 0, pa_fn, &args[i]) != 0) fail("A create");
    }
    for (long i = 0; i < 8; i++) {
        void *r = 0;
        if (pthr_join(th[i], &r) == 0) p->joined++;
        if ((long)r == (i + 1) * 3) p->ret_ok++;
    }
}

/* ---- PJ: the joiner sleeps in the kernel ------------------------------------ */

struct pj { struct pt *p; pthr_t target; };

static void *pj_target(void *a) {
    struct pt *p = (struct pt *)a;
    while (!__atomic_load_n(&p->go, __ATOMIC_SEQ_CST)) yield();
    return (void *)42;
}

static void *pj_joiner(void *a) {
    struct pj *j = (struct pj *)a;
    void *r = 0;
    pthr_join(j->target, &r);
    if ((long)r == 42) j->p->jret_ok = 1;
    __atomic_store_n(&j->p->jdone, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void b_pj(struct pt *p) {
    struct pj j = { p, 0 };
    pthr_t jt;
    if (pthr_create(&j.target, 0, pj_target, p) != 0) fail("J create");
    if (pthr_create(&jt, 0, pj_joiner, &j) != 0) fail("J create");
    p->asleep = blocked(jt->tid, 2000);    /* R2 (a spinning join) is never BLOCKED */
    __atomic_store_n(&p->go, 1, __ATOMIC_SEQ_CST);
    uint64_t end = now() + 2000 * MS;
    while (!__atomic_load_n(&p->jdone, __ATOMIC_SEQ_CST)) {
        if (now() > end) { p->to = 1; return; }
        yield();
    }
    pthr_join(jt, 0);
}

/* ---- PR: 200 create-and-join rounds reclaim their memory ------------------ */

struct pr_ctx { struct pt *p; long me; };

static void *pr_fn(void *a) { (void)a; return 0; }

/* THREADTEST tcbs other than this child and its parent (the probe's main). */
static long pr_others(struct pr_ctx *c) {
    struct { uint32_t pid, ppid, state, flags; char name[16];
             uint64_t run_ticks, dispatches; } pi;
    long n = 0;
    for (long i = 0; i < 4096 && sys(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++) {
        const char *w = "THREADTEST";
        int k = 0;
        while (w[k] && pi.name[k] == w[k]) k++;
        if (w[k] || (long)pi.pid == c->me || (long)pi.pid == c->p->parent) continue;
        n++;
    }
    return n;
}

static long pr_work(void *ctx) {
    struct pr_ctx *c = (struct pr_ctx *)ctx;
    pthr_attr_t at = { PTHR_STACK_MIN, 0 };
    long done = 0;
    for (int i = 0; i < 200; i++) {
        pthr_t t;
        if (pthr_create(&t, &at, pr_fn, 0) != 0) fail("R create");
        if (pthr_join(t, 0) != 0) fail("R join");
        done++;
    }
    c->p->rounds = done;
    /* Let the kernel's reaper free every thread before the meter's settle,
     * so its fingerprint sees none of ours (as arm C does). */
    uint64_t end = now() + 2000 * MS;
    while (pr_others(c) && now() < end) yield();
    return 200;                            /* tcbs created */
}

static void b_pr(struct pt *p) {
    struct pr_ctx c = { p, sys(SYS_GETPID, 0, 0, 0) };
    pthr_attr_t at = { PTHR_STACK_MIN, 0 };
    for (int i = 0; i < 8; i++) {          /* warm-up, as arm C does */
        pthr_t t;
        if (pthr_create(&t, &at, pr_fn, 0) != 0) fail("R warm create");
        pthr_join(t, 0);
    }
    struct pm_result r;
    p->conclusive = pm_measure(pr_work, &c, &r);
    p->drop = r.drop;
    p->trials = r.trials;
    p->rej_pre = r.rej_pre;
    p->rej_post = r.rej_post;
    p->rej_procs = r.rej_procs;
}

/* ---- PD: detached threads are reaped -------------------------------------- */

static void *pd_fn(void *a) {
    struct pt *p = (struct pt *)a;
    while (!__atomic_load_n(&p->dgo, __ATOMIC_SEQ_CST)) yield();
    return 0;
}

static void b_pd(struct pt *p) {
    pthr_attr_t at = { 0, 1 };
    pthr_t th[32];
    for (int i = 0; i < 32; i++) {
        if (pthr_create(&th[i], &at, pd_fn, p) != 0) fail("D create");
        p->detached++;
    }
    __atomic_store_n(&p->dgo, 1, __ATOMIC_SEQ_CST);
    /* A detached block stays mapped until a reap, so its ctid may be read. */
    uint64_t end = now() + 2000 * MS;
    for (int i = 0; i < 32; i++)
        while (__atomic_load_n(&th[i]->ctid, __ATOMIC_ACQUIRE) != 0) {
            if (now() > end) { p->to = 1; return; }
            yield();
        }
    p->reclaimed = pthr_reap();
}

/* ---- PG: the guard page below a thread's stack faults --------------------- */

static void *pg_fn(void *a) {
    (void)a;
    struct pthr *s = pthr_self();
    *(volatile char *)(s->map_base + PTHR_PAGE - 1) = 1;   /* last byte of the guard */
    return 0;
}

static void b_pg(struct pt *p) {
    pthr_t t;
    if (pthr_create(&t, 0, pg_fn, 0) != 0) fail("G create");
    pthr_join(t, 0);
    p->survived = 1;                       /* reached only if the write did not fault */
}

/* ---- PK: keys ------------------------------------------------------------- */

struct pk { struct pt *p; long idx; pthr_key_t k; };

static void pk_dtor(void *v) {
    struct pk *x = (struct pk *)v;
    __atomic_add_fetch(&x->p->dtor, 1, __ATOMIC_SEQ_CST);
}

static void *pk_fn(void *a) {
    struct pk *x = (struct pk *)a;
    place(x->p, x->idx);
    pthr_setspecific(x->k, x);
    pthr_barrier_wait(&x->p->bar);         /* every thread has set before any reads */
    if (pthr_getspecific(x->k) == x) __atomic_add_fetch(&x->p->iso, 1, __ATOMIC_SEQ_CST);
    return 0;                              /* exits with a non-NULL value: one destructor call */
}

static void b_pk(struct pt *p) {
    pthr_key_t k;
    if (pthr_key_create(&k, pk_dtor) != 0) fail("K key");
    pthr_barrier_init(&p->bar, 4);
    struct pk x[4];
    pthr_t th[4];
    for (long i = 0; i < 4; i++) {
        x[i].p = p;
        x[i].idx = i;
        x[i].k = k;
        if (pthr_create(&th[i], 0, pk_fn, &x[i]) != 0) fail("K create");
    }
    for (int i = 0; i < 4; i++) pthr_join(th[i], 0);
    /* Stale: a value set under k1 must not be seen through k2 in the same slot. */
    long mine = 7;
    pthr_setspecific(k, &mine);
    if (pthr_key_delete(k) != 0) fail("K delete");
    pthr_key_t k2;
    if (pthr_key_create(&k2, 0) != 0) fail("K key2");
    p->same_slot = (k2 & 0xFF) == (k & 0xFF);
    p->stale = pthr_getspecific(k2) == &mine;
}

/* ---- PO: once ------------------------------------------------------------- */

static void po_init(void) {
    struct pt *p = (struct pt *)pthr_self()->arg;
    __atomic_add_fetch(&p->runs, 1, __ATOMIC_SEQ_CST);
    linger_tick();                         /* others arrive while it runs */
    __atomic_store_n(&p->odone, 1, __ATOMIC_RELEASE);
}

static void *po_fn(void *a) {
    struct pt *p = (struct pt *)a;
    place(p, __atomic_fetch_add(&p->pidx, 1, __ATOMIC_SEQ_CST) & 3);
    pthr_barrier_wait(&p->bar);
    pthr_once(&p->once, po_init);
    if (__atomic_load_n(&p->odone, __ATOMIC_ACQUIRE) == 1)
        __atomic_add_fetch(&p->waited, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void b_po(struct pt *p) {
    pthr_barrier_init(&p->bar, 4);
    pthr_t th[4];
    for (int i = 0; i < 4; i++)
        if (pthr_create(&th[i], 0, po_fn, p) != 0) fail("O create");
    for (int i = 0; i < 4; i++) pthr_join(th[i], 0);
}

/* ---- driver --------------------------------------------------------------- */

void pthr_arms(void) {
    long pg = pthr__sys6(SYS_MMAP, 0, 4096, PTHR__PROT_RW, 0x01 /* MAP_SHARED */ | PTHR__MAP_ANON, -1, 0);
    if (pg < 0) fail("mmap");
    struct pt *p = (struct pt *)pg;
    long ms, to;
    say("[pthr] start_ms=%d\n", (long)(now() / MS));

    to = run(p, b_pa, 10000, &ms);
    say("[pthr] A ms=%d timeout=%d\n", ms, to);
    say("PRADYOS_PTHR_A joined=%d ret_ok=%d total=%d placed_ok=%d\n",
        (long)p->joined, (long)p->ret_ok, (long)p->ctr, (long)(p->placed == 8));

    to = run(p, b_pj, 8000, &ms);
    say("[pthr] J ms=%d\n", ms);
    say("PRADYOS_PTHR_J asleep=%d ret_ok=%d timeout=%d\n",
        (long)p->asleep, (long)p->jret_ok, to);

    to = run(p, b_pr, 45000, &ms);
    say("[pthr] R drop=%d trials=%d rej_pre=%d rej_post=%d rej_procs=%d ms=%d timeout=%d\n",
        (long)p->drop, (long)p->trials, (long)p->rej_pre, (long)p->rej_post,
        (long)p->rej_procs, ms, to);
    say("PRADYOS_PTHR_R rounds=%d leak_ok=%d conclusive=%d\n",
        (long)p->rounds, (long)(p->conclusive && p->drop <= 32), (long)p->conclusive);

    to = run(p, b_pd, 8000, &ms);
    say("[pthr] D ms=%d\n", ms);
    say("PRADYOS_PTHR_D detached=%d reclaimed=%d timeout=%d\n",
        (long)p->detached, (long)p->reclaimed, to);

    to = run(p, b_pg, 5000, &ms);
    say("[pthr] G survived=%d ms=%d timeout=%d\n", (long)p->survived, ms, to);
    say("PRADYOS_PTHR_G guard_fault=%d\n", (long)(!to && !p->survived));

    to = run(p, b_pk, 8000, &ms);
    say("[pthr] K same_slot=%d ms=%d timeout=%d\n", (long)p->same_slot, ms, to);
    say("PRADYOS_PTHR_K iso_ok=%d dtor=%d stale=%d placed_ok=%d\n",
        (long)p->iso, (long)p->dtor, (long)p->stale, (long)(p->placed == 4));

    to = run(p, b_po, 8000, &ms);
    say("[pthr] O ms=%d timeout=%d\n", ms, to);
    say("PRADYOS_PTHR_O runs=%d waited_ok=%d placed_ok=%d\n",
        (long)p->runs, (long)p->waited, (long)(p->placed == 4));

    say("[pthr] end_ms=%d\n", (long)(now() / MS));
}
