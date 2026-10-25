/* user/include/pagemeter.h — DDR-1185: attributable free-page drop for probes.
 *
 * SYS_SYSINFO.free_pages is system-wide. While the rest of the boot runs, other
 * processes take and return hundreds of pages in bursts: run 40217768311 saw
 * 293 pages go in a 2 s idle window of smoke-thread, and -182 (pages RETURNED)
 * in smoke-shm's private baseline loop. A single before/after pair, or a pair
 * corrected by one idle window measured at another time, therefore reads that
 * traffic as a leak or hides one.
 *
 * pm_measure() runs the workload in trials and accepts a trial only when
 *   1. the free count held within PM_TOL for PM_QUIET_TICKS just before it,
 *   2. after a PM_SETTLE_TICKS settle, it held within PM_TOL for
 *      PM_QUIET_TICKS, and
 *   3. no other process appeared or exited across it (scheduler-ring
 *      fingerprint, the caller and its own children/threads excluded), and
 *   4. no tcb other than the workload's own was CREATED across it. Every tcb
 *      takes the next value of one monotonic counter (sched.c next_tid), so a
 *      throwaway fork before and after the trial brackets the tids handed out
 *      in between; the gap must equal the number of tcbs the workload reports
 *      it created. This catches a foreign process that starts AND exits inside
 *      the trial, which leaves the fingerprint unchanged (CodeRabbit on #90).
 * An accepted trial's drop is the workload's own. If no trial is clean within
 * PM_BUDGET_TICKS the result is inconclusive (returns 0), and the caller must
 * treat that as a FAIL: background traffic never turns into a pass.
 *
 * Header-only and stack-only: a probe may not carry a writable section
 * (DDR-826). Syscalls are issued here so the header does not depend on a
 * probe's own wrappers. */
#ifndef PRADYOS_PAGEMETER_H
#define PRADYOS_PAGEMETER_H

#define PM_TOL            4      /* pages of drift a quiet window may show     */
#define PM_QUIET_TICKS    25     /* 250 ms at the 100 Hz tick                  */
#define PM_SETTLE_TICKS   30     /* 300 ms after the workload, before f1       */
#define PM_BUDGET_TICKS   3000   /* 30 s for all trials, quiet waits included  */
#define PM_TRIALS         6

struct pm_result {
    long drop;        /* accepted trial's free-page drop (positive = pages lost) */
    long trials;      /* trials run, accepted one included                       */
    long rej_pre;     /* trials not started: no quiet window before the deadline */
    long rej_post;    /* rejected: free count moved after the workload           */
    long rej_procs;   /* rejected: another tcb was created, or a process exited  */
};

struct pm_sysinfo {
    char vendor[16]; char brand[64];
    unsigned cpu_count, feat_edx, feat_ecx, pad;
    unsigned long uptime_ticks, free_pages;
};

static inline long pm_sys3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long pm_sys(long n, long a1, long a2) { return pm_sys3(n, a1, a2, 0); }

/* The tid the kernel hands the next tcb: fork a child that exits at once, reap
 * it, return its pid (= its tid). <= 0 on failure. */
static inline long pm_tid_probe(void) {
    long k = pm_sys(15 /* SYS_FORK */, 0, 0);
    if (k == 0) {
        pm_sys(4 /* SYS_EXIT */, 0, 0);
        for (;;) { }
    }
    if (k > 0) {
        int st = 0;
        pm_sys3(16 /* SYS_WAIT4 */, k, (long)&st, 0);
    }
    return k;
}

static inline void pm_sample(unsigned long *ticks, unsigned long *free) {
    struct pm_sysinfo si;
    pm_sys(71 /* SYS_SYSINFO */, (long)&si, 0);
    *ticks = si.uptime_ticks;
    *free  = si.free_pages;
}

/* Sum and count of the pids in the scheduler ring, leaving out the caller and
 * anything whose parent is the caller. Thread tcbs carry parent 0 (sched.c
 * thread create), so a probe's own threads must all be gone when this runs. */
static inline void pm_procs(long me, long *count, long *sum) {
    struct { unsigned pid, ppid, state, flags; char name[16];
             unsigned long run_ticks, dispatches; } pi;
    *count = *sum = 0;
    for (long i = 0; i < 4096 && pm_sys(67 /* SYS_GETPROCS */, i, (long)&pi) == 1; i++) {
        if ((long)pi.pid == me || (long)pi.ppid == me) continue;
        (*count)++;
        *sum += (long)pi.pid;
    }
}

/* Wait until the free count holds within PM_TOL for PM_QUIET_TICKS. Returns
 * the free count at the end of the quiet window, or 0 at the deadline. */
static inline unsigned long pm_quiet(unsigned long deadline) {
    if (deadline) return 0;   /* MUTANT P-INC (DDR-1185): never quiet */
    unsigned long t, f, t0, f0;
    pm_sample(&t0, &f0);
    for (;;) {
        pm_sys(3 /* SYS_YIELD */, 0, 0);
        pm_sample(&t, &f);
        long d = (long)f - (long)f0;
        if (t >= deadline) return 0;    /* the budget first: never start late */
        if (d > PM_TOL || d < -PM_TOL) { t0 = t; f0 = f; }
        else if (t - t0 >= PM_QUIET_TICKS) return f;
    }
}

/* work() runs one trial and returns how many tcbs (forked processes, created
 * threads) it created. Returns 1 with r->drop set when a trial was clean, 0
 * when inconclusive. */
static inline int pm_measure(long (*work)(void *), void *ctx, struct pm_result *r) {
    unsigned long t, f0, f1, f2, deadline;
    long me = pm_sys(2 /* SYS_GETPID */, 0, 0);
    pm_sample(&t, &f0);
    deadline = t + PM_BUDGET_TICKS;
    r->drop = r->trials = r->rej_pre = r->rej_post = r->rej_procs = 0;
    for (int i = 0; i < PM_TRIALS; i++) {
        long p0 = pm_tid_probe();       /* before the quiet wait absorbs it */
        if (p0 <= 0) return 0;
        f0 = pm_quiet(deadline);
        if (!f0) { r->rej_pre++; return 0; }
        long c0, s0, c1, s1;
        pm_procs(me, &c0, &s0);
        pm_sample(&t, &f0);
        r->trials++;
        long own = work(ctx);
        /* Settle: frees the workload causes late (the reaper's tcb and
         * kernel-stack free, deferred unmaps) are the workload's own and must
         * land before f1, not in the post window where they would reject. */
        pm_sample(&t, &f1);
        for (unsigned long s = t + PM_SETTLE_TICKS; t < s; pm_sample(&t, &f1))
            pm_sys(3 /* SYS_YIELD */, 0, 0);
        pm_procs(me, &c1, &s1);
        unsigned long end = t + PM_QUIET_TICKS;
        long dmax = 0;
        do {
            pm_sys(3 /* SYS_YIELD */, 0, 0);
            pm_sample(&t, &f2);
            long d = (long)f2 - (long)f1;
            if (d < 0) d = -d;
            if (d > dmax) dmax = d;
        } while (t < end);
        long p1 = pm_tid_probe();       /* after f1: its own pages are not measured */
        if (c0 != c1 || s0 != s1 || p1 - p0 - 1 != own) { r->rej_procs++; continue; }
        if (dmax > PM_TOL)        { r->rej_post++;  continue; }
        r->drop = (long)f0 - (long)f1;
        return 1;
    }
    return 0;
}

#endif
