/* kernel/sched.c — preemptive round-robin kernel scheduler (Phase 2c/2e). */
#include "sched.h"
#include "kheap.h"
#include "console.h"
#include "tss.h"
#include "syscall.h"
#include "vmm.h"
#include "string.h"            /* memcpy for the per-thread FPU template (5d) */
#include "cpu_mitigations.h"   /* cpu_wrmsr + MSR_IA32_FS_BASE (PROC-D) */
#include "smp.h"               /* rq-3: smp_resched_one (directed wake IPI) */
#include "irq.h"               /* ADR-032: g_ticks for the FS write-budget bucket */

#define STACK_SIZE   16384u
#define QUANTUM      2u           /* ticks per slice (PIT @100Hz -> 20 ms) */

/* Clean x87+SSE state (FNINIT + default MXCSR) captured once in sched_init and
 * copied into every new thread's fpu_state (5d, ADR-023 §D8). A zeroed FXSAVE
 * area is NOT clean — it would load MXCSR=0 (all SSE exceptions unmasked). */
static uint8_t fpu_init_template[FPU_STATE_MAX] __attribute__((aligned(64)));

/* DDR-872 (item 18): 1 once XSAVE is enabled with the AVX/AVX-512 components.
 * Written once on the BSP in sched_init, before any AP runs, so it is read-only
 * by the time a second CPU can observe it — no barrier needed. */
static int g_xsave_on;

/* Which state components XCR0 enables when XSAVE is available.
 * x87(0) | SSE(1) | AVX(2) | opmask(5) | ZMM_hi256(6) | Hi16_ZMM(7).
 * MPX and PKRU are deliberately excluded: nothing in this kernel or its
 * userspace uses them, and every enabled component enlarges the per-thread
 * save area and lengthens the switch. */
#define XCR0_X87        (1ull << 0)
#define XCR0_SSE        (1ull << 1)
#define XCR0_AVX        (1ull << 2)
#define XCR0_OPMASK     (1ull << 5)
#define XCR0_ZMM_HI256  (1ull << 6)
#define XCR0_HI16_ZMM   (1ull << 7)

static inline void fpu_save(void *area) {
    if (g_xsave_on) {
        /* EDX:EAX is the requested-feature bitmap; -1 means "everything XCR0
         * enables". XSAVE writes XSTATE_BV so XRSTOR knows what is present. */
        __asm__ volatile("xsave (%0)"
                         :: "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
                         : "memory");
        return;
    }
    __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
}

static inline void fpu_restore(const void *area) {
    if (g_xsave_on) {
        __asm__ volatile("xrstor (%0)"
                         :: "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
                         : "memory");
        return;
    }
    __asm__ volatile("fxrstor (%0)" :: "r"(area) : "memory");
}

extern void context_switch(uint64_t *save_rsp, uint64_t load_rsp);  /* context.asm */
extern void enter_user_mode(uint64_t rip, uint64_t rsp, uint64_t arg); /* usermode.asm */
extern void signal_sigreturn(struct regs *saved);  /* usermode.asm — full-frame iretq */

#define USER_CS_SEL 0x23u    /* (0x20 | 3): user code64, RPL 3 — matches usermode.asm */
#define USER_SS_SEL 0x1Bu    /* (0x18 | 3): user data,   RPL 3 */

/* current_thread now lives in the percpu area, read via %gs (DDR-SMP-3b,
 * sched.h macro). NULL until sched_init (safe: sched_tick checks). */
#include "spinlock.h"
static spinlock_t g_sched_lock = SPINLOCK_INIT;   /* DDR-SMP-3c-locks-1 */
/* cap-2b: one idle per CPU. The BSP idle is static (small, like the pre-cap-2b
 * single idle); AP idles are kmalloc'd in sched_ap_enter — a full struct tcb
 * (~KB) times PERCPU_MAX in BSS would blow the low-mem image cap (big tables
 * come from the heap/PMM pool, never BSS). g_idle[cpu] is the fallback target. */
static struct tcb idle0;
static struct tcb *g_idle[PERCPU_MAX];
/* cap-2b: APs are brought online (smp_start_aps) BEFORE sched_init runs, so an
 * AP must not touch the ring until the scheduler exists. Set (release) at the
 * end of sched_init; sched_ap_enter waits on it (acquire). */
static volatile int g_sched_ready;
/* cap-4 proof: set by schedule() when an AP claims a ring-3 thread; polled by
 * the boot proof (which prints — schedule() itself must not, under the lock). */
volatile int g_user_on_ap;

/* DDR-SMP-rq-1: per-CPU ready queues (FIFO, intrusive via tcb.rq_next). The
 * rq lock is a LEAF: never held while taking g_sched_lock or another rq lock
 * (steal releases its own first, then TRYlocks victims — no hold-and-wait). */
struct rq {
    spinlock_t  lock;
    struct tcb *head, *tail;
};
static struct rq g_rq[PERCPU_MAX];

/* Enqueue t on cpu's ready FIFO. Idempotent ACROSS queues (DDR-736): the rq_on
 * claim is an atomic exchange taken BEFORE any queue lock, because two
 * concurrent pushers use two DIFFERENT leaf locks — a waker's sched_unblock
 * (device-IRQ completion on CPU B) racing the blocker's own schedule() re-queue
 * (CPU A, which sees the waker's READY CAS in prev->state). Under the old
 * per-queue check both could read rq_on == 0 and link ONE tcb into TWO FIFOs
 * through its single rq_next pointer, breaking rq-2's exclusion premise ("a
 * thread sits in exactly one queue"): list corruption loses threads (hang) or
 * two CPUs pop and double-run one kernel stack (heap corruption/double free) —
 * both observed in CI. The loser simply no-ops: the winner's queue holds the
 * thread, findable by that CPU's pick or any steal. */
static void rq_push(int cpu, struct tcb *t) {
    if (__atomic_exchange_n(&t->rq_on, 1, __ATOMIC_ACQ_REL))
        return;                        /* already queued somewhere — one queue only */
    struct rq *q = &g_rq[cpu];
    uint64_t fl = spin_lock_irqsave(&q->lock);
    t->rq_next = 0;
    if (q->tail) q->tail->rq_next = t; else q->head = t;
    q->tail = t;
    spin_unlock_irqrestore(&q->lock, fl);
}

/* Pop the first pickable thread from cpu's own FIFO (skips became-unpickable
 * entries, e.g. a thread that blocked after being enqueued). NULL if empty. */
/* Dequeue helper: pops entries until a READY one is found; non-READY entries
 * are stale and drop. Caller holds q->lock.
 * rq-2: on_cpu is NO LONGER part of the filter. Exclusion is the DEQUEUE (a
 * thread sits in exactly one queue, so exactly one CPU pops it); on_cpu now
 * only means "still executing / rsp not yet saved", and the picker
 * switch_wait_offcpu()s on it before loading that rsp. This also retires
 * rq-1's transient re-append: a READY-but-still-on-CPU thread (an unblock
 * racing its owner's switch-away) is now legitimately takeable. */
static struct tcb *rq_take(struct rq *q) {
    struct tcb *t;
    while ((t = q->head)) {
        q->head = t->rq_next;
        if (!q->head) q->tail = 0;
        t->rq_next = 0;
        /* DDR-736: RELEASE so the next rq_push claimant (possibly another CPU
         * not holding THIS queue's lock) observes the unlink before its claim. */
        __atomic_store_n(&t->rq_on, 0, __ATOMIC_RELEASE);
        if (t->state == THREAD_READY)
            return t;
    }
    return 0;
}

static struct tcb *rq_pop(int cpu) {
    struct rq *q = &g_rq[cpu];
    uint64_t fl = spin_lock_irqsave(&q->lock);
    struct tcb *t = rq_take(q);
    spin_unlock_irqrestore(&q->lock, fl);
    return t;
}

/* rq-2 D2: run in the RESUMED thread's context, right after context_switch.
 * `this_cpu()->prev` names the thread the CPU we are NOW on just switched away
 * from — its stack is saved, so publish it off-CPU with a RELEASE store. This
 * pairs with the acquire spins in switch_wait_offcpu / sched_free_tcb. */
static inline void finish_task_switch(void) {
    struct percpu *pc = this_cpu();
    if (pc && pc->prev) {
        struct tcb *p = pc->prev;
        pc->prev = 0;
        __atomic_store_n(&p->on_cpu, -1, __ATOMIC_RELEASE);
    }
}

/* rq-2 D3: a picked thread may still be mid-switch-away on another CPU (its
 * rsp not yet saved). Wait for its owner's release before we load that rsp.
 * Bounded: the holder is executing a few instructions, never blocked on us. */
static inline void switch_wait_offcpu(struct tcb *t) {
    while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) >= 0)
        __asm__ volatile("pause");
}

/* rq-3: a lockless hint — is any CPU's ready queue non-empty? A stale read is
 * harmless: a false negative is caught by the timer tick, a false positive just
 * costs one extra idle-loop iteration. Used only to close the wake/idle race. */
static int rq_has_ready(void) {
    for (int c = 0; c < PERCPU_MAX; c++)
        if (g_rq[c].head)
            return 1;
    return 0;
}

/* Steal one thread from another CPU's FIFO (trylock; own rq NOT held). */
static struct tcb *rq_steal(int self) {
    for (int c = 0; c < PERCPU_MAX; c++) {
        if (c == self || !g_rq[c].head)
            continue;
        struct rq *q = &g_rq[c];
        if (__atomic_test_and_set(&q->lock.v, __ATOMIC_ACQUIRE))
            continue;                    /* busy — don't convoy, next victim */
        struct tcb *t = rq_take(q);      /* transients re-appended, never dropped */
        spin_unlock(&q->lock);
        if (t)
            return t;
    }
    return 0;
}
static uint32_t next_tid = 1;
static uint32_t g_init_pid = 0;   /* 5d: PID 1; orphans reparent here, exit panics */

void sched_set_init_pid(uint32_t pid) { g_init_pid = pid; }

/* DDR-SMP-3c-locks-1: the scheduler's interrupt masking IS the scheduler
 * spinlock (irqsave variant — identical one-CPU semantics, cross-CPU exclusion
 * added). Defined here (not at schedule()) because sched_create's ring insert
 * and the topology paths (cap-2a) take it too. schedule() holds it ACROSS
 * context_switch; the resuming thread's irq_restore releases it (the switch-lock
 * handoff — test-and-set locks have no owner). A brand-new thread's first entry
 * has no resumed frame, so thread_trampoline releases the lock itself. */
static inline uint64_t irq_save(void) {
    return spin_lock_irqsave(&g_sched_lock);
}
static inline void irq_restore(uint64_t f) {
    spin_unlock_irqrestore(&g_sched_lock, f);
}

/* rq-2: the SCHEDULE hot path no longer takes g_sched_lock at all — it only
 * masks local IRQs across the switch (context_switch preserves each thread's
 * RFLAGS, so a resumed thread comes back masked and restores below).
 * g_sched_lock now covers ring TOPOLOGY only (D5). */
static inline uint64_t local_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void local_irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

static void schedule(void);        /* fwd decl: thread_trampoline schedules on exit */

/* First code a freshly-created thread runs (entered via context_switch's RET).
 * current_thread is already the new thread (set by schedule before switching). */
static void thread_trampoline(void) {
    /* rq-2: a brand-new thread has no resumed schedule() frame, so IT performs
     * the finish_task_switch — releasing the thread this CPU just switched away
     * from (its stack is saved; context_switch has left it). The crafted initial
     * RFLAGS already restored IF via context_switch's popfq. (Before rq-2 this
     * released g_sched_lock, which the switch no longer holds.) */
    finish_task_switch();
    current_thread->entry(current_thread->arg);
    /* cap-2b D3: cooperative exit. A returning kernel thread marks itself DONE
     * and schedules away — DONE is unpickable so it never runs again. The old
     * for(;;)hlt relied on timer preemption to leave, which an un-preempted AP
     * (cap-3 pending) never gets, wedging that CPU.
     *
     * rq-2 invariant: `on_cpu` MUST NOT be cleared here. This thread is still
     * executing on its own kernel stack until schedule()->context_switch saves
     * its rsp; clearing on_cpu now would let switch_wait_offcpu()/sched_free_tcb()
     * proceed against a not-yet-saved rsp and a live stack (a use-after-free that
     * surfaces as heap corruption / kfree double-free under an SMP thread storm).
     * It is released with a RELEASE store by finish_task_switch() in whichever
     * thread next resumes on this CPU — strictly after the switch has left this
     * stack — exactly as sched_exit() relies on for ZOMBIEs. */
    current_thread->state = THREAD_DONE;
    schedule();
    for (;;)                        /* unreachable */
        __asm__ volatile("hlt");
}

/* Shared idle-thread init (cap-2b: one per CPU). The FPU template is captured
 * once by sched_init before any init_idle runs. */
static void init_idle(struct tcb *idle, int cpu) {
    memset(idle, 0, sizeof(*idle));    /* zero all fields (AP idles are kmalloc'd) */
    idle->name = "idle";
    idle->state = THREAD_RUNNING;
    idle->quantum = idle->quantum_reset = QUANTUM;
    idle->caps = cap_table_create();
    idle->is_idle = 1;
    idle->on_cpu = cpu;                /* tid/is_user/cr3/fs_base = 0 via memset */
    memcpy(idle->fpu_state, fpu_init_template, sizeof idle->fpu_state);
}

void sched_init(void) {
    /* Capture a clean FPU template (SSE already enabled by cpu_enable_sse in
     * kmain, which runs before sched_init). New threads + idle copy this so the
     * first FXRSTOR loads a valid state, not zeros. */
    uint32_t mxcsr = 0x1F80u;          /* round-nearest, all exceptions masked */
    __asm__ volatile("fninit");
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));

    /* DDR-872 (item 18): enable XSAVE with the AVX/AVX-512 components when the
     * CPU has them, so a context switch preserves YMM/ZMM. Without this the
     * switch saves only x87+XMM via FXSAVE, and any kernel or ring-3 use of the
     * wide registers is silently corrupted across a preemption — which is why
     * DDR-871 could not use AVX in fast_memcpy.
     *
     * Order matters: CR4.OSXSAVE must be set before XSETBV, and XCR0 must be
     * set before the size query, because CPUID.0xD.0:EBX reports the size for
     * the components CURRENTLY enabled. Querying first would size the area for
     * x87+SSE and then enable more state than it can hold. */
    uint32_t xa = 0, xb = 0, xc = 0, xd = 0;
    __asm__ volatile("cpuid" : "=a"(xa), "=b"(xb), "=c"(xc), "=d"(xd)
                             : "a"(1), "c"(0));
    int have_xsave = (xc & (1u << 26)) != 0;          /* CPUID.1:ECX.XSAVE */

    if (have_xsave) {
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ull << 18);                          /* CR4.OSXSAVE */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

        /* Only enable components the CPU actually supports: XSETBV #GPs on a
         * bit the CPU does not implement, which would fault the boot path. */
        __asm__ volatile("cpuid" : "=a"(xa), "=b"(xb), "=c"(xc), "=d"(xd)
                                 : "a"(0xD), "c"(0));
        uint64_t supported = ((uint64_t)xd << 32) | xa;
        uint64_t want = XCR0_X87 | XCR0_SSE;
        if (supported & XCR0_AVX)       want |= XCR0_AVX;
        if ((supported & XCR0_OPMASK) && (supported & XCR0_ZMM_HI256) &&
            (supported & XCR0_HI16_ZMM))
            want |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;

        uint32_t lo = (uint32_t)want, hi = (uint32_t)(want >> 32);
        __asm__ volatile("xsetbv" :: "c"(0), "a"(lo), "d"(hi));

        /* Now the size is meaningful. */
        __asm__ volatile("cpuid" : "=a"(xa), "=b"(xb), "=c"(xc), "=d"(xd)
                                 : "a"(0xD), "c"(0));
        if (xb > FPU_STATE_MAX) {
            /* PANIC rather than truncate. A short XSAVE area is not a smaller
             * save — XSAVE writes past the end of the tcb and corrupts whatever
             * follows it, which would surface anywhere but here. */
            kputs("[fpu] PANIC: XSAVE area ");
            kputdec(xb);
            kputs(" exceeds FPU_STATE_MAX ");
            kputdec(FPU_STATE_MAX);
            kputs("\r\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
        g_xsave_on = 1;
        kputs("[fpu] XSAVE on, area ");
        kputdec(xb);
        kputs(" bytes\r\n");
    } else {
        kputs("[fpu] XSAVE absent - FXSAVE (x87+SSE only)\r\n");
    }

    /* Build the per-thread init template with the SAME instruction the switch
     * will restore with. An FXSAVE image is not a valid XSAVE area, so mixing
     * them would XRSTOR garbage into the first thread that runs. */
    if (g_xsave_on) {
        /* A zeroed area is exactly right here: XSTATE_BV = 0 tells XRSTOR to
         * put every component in its INIT state, and XCOMP_BV = 0 selects the
         * non-compacted format XSAVE produces. That is cleaner than capturing a
         * live image, which would bake in whatever the BSP happened to hold. */
        memset(fpu_init_template, 0, sizeof fpu_init_template);
    } else {
        /* FXSAVE path: a zeroed area is NOT clean — it loads MXCSR = 0, which
         * unmasks every SSE exception. Capture a real FNINIT image instead. */
        __asm__ volatile("fxsave (%0)" :: "r"(fpu_init_template) : "memory");
    }

    struct tcb *idle = &idle0;          /* the BSP idle + ring anchor */
    init_idle(idle, 0);
    g_idle[0] = idle;
    idle->next = idle;                 /* ring of one */
    current_thread = idle;
    __atomic_store_n(&g_sched_ready, 1, __ATOMIC_RELEASE);   /* APs may now join */
}

/* cap-2b: an AP joins the scheduler. Sets up this CPU's idle, links it into the
 * shared ring, and runs the idle loop — drain the directed mailbox (smp_run_on),
 * schedule any READY ring thread, then sleep until the next interrupt. The
 * caller (smp_ap_entry) has already loaded this AP's IDT + enabled its LAPIC.
 * Never returns. */
void sched_ap_enter(void) {
    struct percpu *pc = this_cpu();
    int cpu = pc ? (int)pc->cpu_idx : 0;

    /* The scheduler is initialized AFTER APs come online. Until it is, behave
     * like the old park loop — drain directed mailbox jobs (the boot-time
     * job-dispatch test runs before sched_init) and sleep. The acquire pairs
     * with sched_init's release so g_idle[0] + the ring are visible below. */
    while (!__atomic_load_n(&g_sched_ready, __ATOMIC_ACQUIRE)) {
        void (*fn)(void) = __atomic_load_n(&pc->job, __ATOMIC_ACQUIRE);
        if (fn) {
            fn();
            __atomic_store_n(&pc->job, (void (*)(void))0, __ATOMIC_RELEASE);
        }
        __asm__ volatile("sti; hlt");
    }

    struct tcb *idle = (struct tcb *)kmalloc(sizeof(struct tcb));  /* heap, not BSS */
    if (!idle)
        for (;;) __asm__ volatile("cli; hlt");   /* no idle -> this CPU cannot schedule */
    init_idle(idle, cpu);
    g_idle[cpu] = idle;

    /* Link this CPU's idle after the BSP idle anchor and adopt it as current —
     * topology, under the scheduler lock. The idle's rsp is left unseeded: it is
     * first written by context_switch's save when this CPU first switches away. */
    uint64_t fl = irq_save();
    idle->next = g_idle[0]->next;
    g_idle[0]->next = idle;
    current_thread = idle;             /* this_cpu()->current */
    irq_restore(fl);

    for (;;) {
        void (*fn)(void) = __atomic_load_n(&pc->job, __ATOMIC_ACQUIRE);
        if (fn) {                      /* directed job from smp_run_on */
            fn();
            __atomic_store_n(&pc->job, (void (*)(void))0, __ATOMIC_RELEASE);
        }
        schedule();                    /* run any READY kernel thread from the ring */
        /* rq-3: advertise idle so a waker will kick us; then DOUBLE-CHECK for
         * work that may have arrived between schedule() and now (else that
         * waker saw idle==0 and skipped the kick). SEQ_CST orders the flag store
         * before the rq_has_ready load, pairing with the waker's rq_push then
         * idle read. If we still hlt, the wake IPI (or timer) breaks it. */
        __atomic_store_n(&pc->idle, 1, __ATOMIC_SEQ_CST);
        if (rq_has_ready()) {
            __atomic_store_n(&pc->idle, 0, __ATOMIC_SEQ_CST);
            continue;                  /* work appeared — steal it next schedule() */
        }
        __asm__ volatile("sti; hlt");  /* sleep until wake IPI / timer */
        __atomic_store_n(&pc->idle, 0, __ATOMIC_SEQ_CST);
    }
}

/* Core creator. `initial_state` is READY for kernel threads (fully runnable at
 * insert — no post-init) and BLOCKED for user threads (the caller sets
 * cr3/user_rip/authority, THEN sched_unblock — closing the create-then-init race
 * against a second scheduling CPU; DDR-SMP-3c-cap-2a D3). */
static struct tcb *sched_create_state(thread_fn entry, void *arg, const char *name,
                                      uint32_t initial_state) {
    struct tcb *t = (struct tcb *)kmalloc(sizeof(struct tcb));
    if (!t)
        return 0;
    uint64_t base = (uint64_t)(uintptr_t)kmalloc(STACK_SIZE);
    if (!base) {
        kfree(t);
        return 0;
    }

    t->kstack_base = base;
    t->tid = next_tid++;
    t->state = initial_state;
    t->on_cpu = -1;                  /* not running anywhere until schedule() claims it */
    t->is_idle = 0;                  /* only the per-CPU idles set this */
    t->rq_next = 0;                  /* rq-1: not enqueued yet */
    t->rq_on = 0;
    t->quantum = t->quantum_reset = QUANTUM;
    t->entry = entry;
    t->arg = arg;
    t->name = name;
    t->name_buf[0] = 0;             /* DDR-756: empty until SYS_SETNAME (kmalloc !zero) */
    t->caps = cap_table_create();
    t->fs_write_budget = FS_WRITE_BUDGET_DEFAULT;   /* ADR-032: full bucket */
    t->fs_budget_tick  = g_ticks;                   /* kmalloc !zero */
    t->is_user = 0;            /* kmalloc does not zero — init the user fields */
    t->pid = 0;
    t->user_rip = 0;
    t->user_stack = 0;
    t->user_arg = 0;
    t->cr3 = 0;                /* kernel master AS until a loader assigns one */
    fd_table_init(&t->fdt);    /* all fds free; user threads get stdio wired below */
    t->root_mnt = -1;          /* no filesystem root until a loader assigns one */
    t->fs_cap = 0;
    for (int i = 0; i < VM_AREA_MAX; i++) { t->vma[i].base = 0; t->vma[i].npages = 0; }
    t->mmap_next = VMM_MMAP_BASE;   /* anonymous mmap bump pointer */
    t->parent_pid = 0;             /* set by sched_create_user_clone for forks */
    t->fork_retval = -1;           /* unset until a fork assigns it             */
    t->exit_status = 0;            /* set by sched_exit, collected by wait4     */
    t->waiter = 0;                 /* parent blocked in wait4 on this thread    */
    t->sig_pending = 0;            /* PROC-C: no pending signals                */
    for (int i = 0; i < 32; i++)
        t->sig_handlers[i] = 0;
    t->sig_active = 0;
    t->fs_base = 0;                /* PROC-D: no thread pointer until SYS_SET_TLS */
    t->forked = 0;                 /* 5e: normal launch unless a fork sets fork_regs */
    t->is_agent = 0;               /* L6: not an AETHER agent unless the spawner sets it */
    t->is_sovereign = 0;           /* L6: no CAP_SOVEREIGN unless the kernel grants it    */
    t->is_net = 0;                 /* DDR-731: no CAP_NET unless the spawner grants it    */
    t->is_memory = 0;              /* DDR-836: no CAP_MEMORY unless granted; kmalloc does
                                    * not zero, so every new field needs this line    */
    t->checkpointed = 0;           /* DDR-837: not frozen                             */
    t->agent_depth = 0;            /* DDR-838: outside any agent lineage by default    */
    t->is_rewrite = 0;             /* DDR-842: no CAP_REWRITE unless granted           */
    t->run_ticks = 0;              /* DDR-735: CPU accounting starts at zero              */
    t->dispatches = 0;
    t->mem_limit = 0;              /* L6: 0 -> lazy 128 MiB cap (aether_mem)              */
    t->mem_used = 0;
    t->sc_count = 0;               /* L6: syscall rate-limit window (agents only)         */
    t->sc_window_start = 0;
    memcpy(t->fpu_state, fpu_init_template, sizeof t->fpu_state);  /* 5d: clean FPU */

    /* Seed the stack with a context_switch frame whose RET enters the
     * trampoline, with RFLAGS = IF set so the thread runs interruptible. */
    uint64_t *sp = (uint64_t *)(uintptr_t)(base + STACK_SIZE);
    *--sp = (uint64_t)(uintptr_t)thread_trampoline;  /* return address */
    *--sp = 0;            /* rbx */
    *--sp = 0;            /* rbp */
    *--sp = 0;            /* r12 */
    *--sp = 0;            /* r13 */
    *--sp = 0;            /* r14 */
    *--sp = 0;            /* r15 */
    *--sp = 0x202;        /* rflags: IF | reserved bit 1 */
    t->rsp = (uint64_t)(uintptr_t)sp;

    /* Insert into the ring after the current thread — topology, under the
     * scheduler lock (cap-2a D2) so a concurrent CPU's walk/insert can't corrupt
     * the links. Uncontended on one CPU. */
    uint64_t fl = irq_save();
    t->next = current_thread->next;
    current_thread->next = t;
    irq_restore(fl);
    /* rq-1: a READY-at-insert (kernel) thread goes straight onto this CPU's
     * ready FIFO; BLOCKED user threads are enqueued by their sched_unblock. */
    if (initial_state == THREAD_READY) {
        struct percpu *pc = this_cpu();
        rq_push(pc ? (int)pc->cpu_idx : 0, t);
    }
    return t;
}

struct tcb *sched_create(thread_fn entry, void *arg, const char *name) {
    return sched_create_state(entry, arg, name, THREAD_READY);
}

/* Kernel-side launch for a ring-3 thread: set the ring-0 stack the CPU will use
 * for this thread's syscalls/interrupts, then drop to user mode (never returns). */
static void user_launch(void *arg) {
    (void)arg;
    struct tcb *t = current_thread;
    uint64_t ktop = t->kstack_base + STACK_SIZE;
    tss_set_rsp0(ktop);
    this_cpu()->kstack_top = ktop;    /* SYSCALL stack switch reads [gs:16] (3b) */
    if (t->forked)
        signal_sigreturn(&t->fork_regs);   /* resume with the parent's full reg set, RAX=0 */
    enter_user_mode(t->user_rip, t->user_stack, t->user_arg);
}

struct tcb *sched_create_user(const char *name, uint64_t user_rip, uint64_t user_stack) {
    /* DDR-boot-authority-race + cap-2a D3: created BLOCKED *at insert* (atomic
     * under the ring lock), so no CPU can run it before the loader's caller
     * grants authority (is_sovereign/is_agent) and THEN sched_unblock()s it. */
    struct tcb *t = sched_create_state(user_launch, 0, name, THREAD_BLOCKED);
    if (!t)
        return 0;
    t->is_user = 1;
    t->pid = t->tid;
    t->user_rip = user_rip;
    t->user_stack = user_stack;
    fd_init_std(&t->fdt);      /* stdin/stdout/stderr -> console */
    return t;
}

struct tcb *sched_create_user_clone(struct tcb *parent, uint64_t child_cr3,
                                    uint64_t entry, uint64_t user_rsp) {
    /* cap-4: the syscall snapshot is per-CPU now (this_cpu()->u_*, written by
     * syscall_entry.asm), so another CPU's syscalls can't clobber it. The cli
     * guard stays anyway: a LOCAL preemption could migrate this thread to
     * another CPU mid-fork, and the snapshot must be read on the CPU that took
     * the syscall. BLOCKED-create (cap-2a D3) keeps the child unpickable until
     * fully built. */
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    struct percpu *pc = this_cpu();

    struct tcb *t = sched_create_state(user_launch, 0, parent->name, THREAD_BLOCKED);
    if (t) {
        t->is_user     = 1;
        t->pid         = t->tid;
        t->parent_pid  = parent->pid;
        t->user_rip    = entry;
        t->user_stack  = user_rsp;
        t->user_arg    = 0;               /* child RDI=0; RAX=0 (fork ret) via enter_user_mode */
        t->cr3         = child_cr3;
        t->fork_retval = 0;
        t->root_mnt    = parent->root_mnt;
        t->fs_cap      = parent->fs_cap;  /* valid: cap_fork copies the table verbatim */
        t->fs_base     = parent->fs_base; /* PROC-D: child inherits the thread pointer */
        /* DDR-838: lineage, not authority. is_agent is deliberately NOT inherited
         * (see the zeroing in sched_create), so the containment cap has to ride
         * on something fork does propagate. A child of an agent, or of anything
         * already inside an agent lineage, is one generation deeper. */
        t->agent_depth = (parent->is_agent || parent->agent_depth > 0)
                       ? parent->agent_depth + 1 : 0;
        memcpy(t->fpu_state, parent->fpu_state, sizeof t->fpu_state);  /* 5d: inherit FPU */
        /* 5e: full-register fork. Resume the child with the parent's complete
         * register frame (callee-saved snapshot from syscall entry) and RAX=0,
         * via signal_sigreturn — so non-inlined code keeps a valid RBP/RBX/R12-15.
         * Caller-saved regs (RDI/RSI/RDX/RCX/R8-R11) are 0: per the syscall ABI the
         * parent's post-`syscall` code does not rely on them. */
        t->forked = 1;
        struct regs *fr = &t->fork_regs;
        memset(fr, 0, sizeof *fr);
        fr->rbx = pc->u_rbx;  fr->rbp = pc->u_rbp;
        fr->r12 = pc->u_r12;  fr->r13 = pc->u_r13;
        fr->r14 = pc->u_r14;  fr->r15 = pc->u_r15;
        fr->rax = 0;                              /* fork returns 0 in the child */
        fr->rip = entry;                          /* = this CPU's u_rip          */
        fr->rsp = user_rsp;                       /* = this CPU's u_rsp          */
        fr->rflags = pc->u_rflags;                /* parent's flags (IF set)     */
        fr->cs = USER_CS_SEL;  fr->ss = USER_SS_SEL;
        /* Replace the fresh (empty) cap + fd tables with copies of the parent's. */
        if (cap_fork(parent->caps, t->caps) != 0 || fd_clone(parent, t) != 0) {
            sched_destroy(t);
            t = 0;
        }
    }

    if (t)
        sched_unblock(t);      /* fully built: now runnable on any CPU (cap-2a D3) */

    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
    return t;
}

/* Unlink t from the circular ready ring. Caller MUST hold g_sched_lock
 * (cap-2a D2 — topology mutation is now serialized across CPUs). */
static void sched_ring_unlink(struct tcb *t) {
    struct tcb *p = t->next;
    while (p->next != t)
        p = p->next;
    p->next = t->next;
}

/* Free a thread's per-process resources (open files, cap table, kstack, TCB).
 * Takes NO scheduler lock — kfree/fd_free take their own; t must already be
 * unlinked and not running anywhere. Freeing outside g_sched_lock keeps that
 * leaf lock short (the reaper otherwise held it across vmm teardown). */
static void sched_free_tcb(struct tcb *t) {
    /* rq-2 D4: the victim may still be executing on its kernel stack (a ZOMBIE
     * mid-final-switch). Wait for its owner CPU's finish_task_switch release
     * before freeing that stack — this is what replaces rq-1's
     * lock-held-across-the-exit-switch (DDR-SMP-exit-stack-race). */
    switch_wait_offcpu(t);
    for (int i = 0; i < FD_MAX; i++)
        fd_free(t, i);
    if (t->caps)
        cap_table_destroy(t->caps);
    if (t->kstack_base)
        kfree((void *)(uintptr_t)t->kstack_base);
    kfree(t);
}

/* DDR-743: snapshot the index-th ring thread for SYS_GETPROCS. Walks the
 * circular ring under g_sched_lock (create/exit/reap mutate it on any CPU),
 * copies pure values into *out. A create/exit racing between indices only
 * adds/drops a row — fine for a best-effort `ps`. */
int sched_snapshot(int index, struct procinfo *out) {
    if (index < 0 || !current_thread)
        return 0;
    uint64_t fl = irq_save();
    struct tcb *t = current_thread;
    int i = 0, found = 0;
    do {
        if (i == index) {
            out->pid   = t->pid;
            out->ppid  = t->parent_pid;
            out->state = t->state;
            out->flags = t->is_user ? 1u : 0u;
            int k = 0;
            if (t->name)
                for (; k < 15 && t->name[k]; k++) out->name[k] = t->name[k];
            out->name[k] = 0;
            out->run_ticks  = t->run_ticks;    /* DDR-754: CPU accounting */
            out->dispatches = t->dispatches;
            found = 1;
            break;
        }
        i++;
        t = t->next;
    } while (t != current_thread);
    irq_restore(fl);
    return found;
}

void sched_destroy(struct tcb *t) {
    if (!t || t == current_thread)
        return;
    uint64_t fl = irq_save();          /* unlink under the lock ... */
    sched_ring_unlink(t);
    irq_restore(fl);
    sched_free_tcb(t);                  /* ... free outside it */
}

static int runnable(const struct tcb *t) {
    return t->state == THREAD_READY || t->state == THREAD_RUNNING;
}

/* rq-1: the old ring-walk pickable() is gone — the rq pop/steal paths filter
 * inline (READY && on_cpu<0; idles are never enqueued at all). USER threads
 * run on any CPU since cap-4. */

/* schedule() must be atomic against the timer: it is reached both voluntarily
 * (yield/sched_block) and from the timer IRQ (sched_tick). If a voluntary
 * schedule() ran with interrupts enabled (e.g. the yield in a driver's
 * busy-wait), a timer tick could re-enter schedule() mid-context-switch and
 * corrupt thread state. Masking interrupts here makes it non-reentrant; the
 * per-thread IF is preserved across the switch by context_switch (pushfq/popfq)
 * and the save/restore below restores the caller's IF on resume. */
/* irq_save/irq_restore (the g_sched_lock acquire/release) are defined near the
 * top of the file — sched_create and the topology paths use them too. */

/* Switch to the next runnable thread in the ring (round-robin, skipping
 * blocked/finished threads). The idle thread is always runnable, so there is
 * always something to run.
 * schedule_locked: the body, entered with g_sched_lock ALREADY HELD (`fl` from
 * the caller's irq_save). Exists for sched_exit (DDR-SMP-exit-stack-race): the
 * dying thread must hold the lock ACROSS its final context_switch so a
 * collector on another CPU cannot free its kernel stack until the handoff
 * release — which happens strictly after the switch has left that stack. */
static void schedule_locked(uint64_t fl) {
    struct tcb *prev = current_thread;
    struct percpu *pc = this_cpu();
    int cpu = pc ? (int)pc->cpu_idx : 0;

    /* rq-1: O(1) pick — own FIFO first, then steal; no more ring scan. Safe
     * against the resume-before-save hazard because the switch below still
     * runs under g_sched_lock (held here): a thief that pops `prev` right
     * after we re-enqueue it below still serializes its own switch behind this
     * lock's handoff release, by which time context_switch has saved prev->rsp
     * (the exit-stack-race argument, applied to preemption). */
    struct tcb *next = rq_pop(cpu);
    if (!next)
        next = rq_steal(cpu);
    if (!next) {
        /* Empty queues. The own idle is never enqueued but IS a real context
         * (the BSP idle runs sched_demo/main work): a non-idle prev must still
         * round-robin THROUGH it (prev re-queues below), or a yield loop would
         * starve it — the cap-2b lesson, rq edition. Only the idle itself may
         * "keep running" here. */
        if (prev == g_idle[cpu] && runnable(prev)) {
            local_irq_restore(fl);
            return;
        }
        next = g_idle[cpu];
    }

    /* rq-2: prev's on_cpu STAYS set until its stack is saved — it is released
     * by finish_task_switch() in whichever thread resumes on this CPU. A thief
     * that pops prev from the queue below spins in switch_wait_offcpu() until
     * then, so it can never load a stale prev->rsp. Exclusion against
     * double-run is the dequeue, not on_cpu (rq_take). */
    if (prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;
    if (prev->state == THREAD_READY && !prev->is_idle)
        rq_push(cpu, prev);        /* rq-1: a preempted-but-runnable prev re-queues */

    /* next may still be mid-switch-away on another CPU: wait for its release
     * before we touch its rsp. (next != prev here — the keep-running case
     * returned above, so we never wait on ourselves.) */
    switch_wait_offcpu(next);
    next->on_cpu = cpu;
    next->state = THREAD_RUNNING;
    next->dispatches++;            /* DDR-735: switch-in count (under the claim) */
    if (next->is_user && pc && !pc->is_bsp)
        g_user_on_ap = 1;          /* cap-4 proof: a ring-3 thread claimed by an AP
                                    * (flag only — no console I/O under the lock) */
    current_thread = next;
    if (next->is_user) {       /* point the CPU at this thread's ring-0 stack */
        uint64_t ktop = next->kstack_base + STACK_SIZE;
        tss_set_rsp0(ktop);
        this_cpu()->kstack_top = ktop;    /* SYSCALL stack switch reads [gs:16] (3b) */
        /* PROC-D (ADR-023): restore this thread's FS base. tcb->fs_base is the sole
         * authority (only SYS_SET_TLS changes it), so restore-on-switch-in is both
         * necessary and sufficient — no save in context_switch is needed. */
        cpu_wrmsr(MSR_IA32_FS_BASE, next->fs_base);
    }
    /* Switch address spaces if the next thread lives in a different one. Kernel
     * stacks are identity-mapped in every AS, so this is safe before the stack
     * switch in context_switch. cr3 == 0 means the kernel master AS. */
    uint64_t kmaster = vmm_kernel_cr3();
    uint64_t prev_cr3 = prev->cr3 ? prev->cr3 : kmaster;
    uint64_t next_cr3 = next->cr3 ? next->cr3 : kmaster;
    if (next_cr3 != prev_cr3)
        __asm__ volatile("mov %0, %%cr3" :: "r"(next_cr3) : "memory");
    /* 5d (ADR-023 §D8) + DDR-740: per-thread FPU save/restore, but only ACROSS
     * user threads. The kernel is -mgeneral-regs-only (no SSE even in string
     * ops), so kernel threads never touch the x87/SSE register file — saving or
     * restoring it for a kernel-involved switch is pure waste (two 512-byte
     * FX ops). Save the outgoing state only if it may be dirty (prev is a user
     * thread); restore the incoming state only if it will be read (next is a
     * user thread). A user thread thus always sees its own state (U->K saves U;
     * K->V restores V), while kernel threads — which cannot observe the register
     * file — are skipped. smoke-fpu (two ring-3 XMM users) is the correctness
     * gate. Nothing between here and context_switch touches the FPU. */
    if (prev->is_user)
        fpu_save(prev->fpu_state);
    if (next->is_user)
        fpu_restore(next->fpu_state);
    /* rq-2 D2: name the outgoing thread for whoever resumes on THIS CPU. */
    if (pc)
        pc->prev = prev;
    context_switch(&prev->rsp, next->rsp);
    /* Resumed here later (possibly on a different CPU) as some earlier `prev`.
     * Release the thread THAT CPU just switched away from, then unmask. */
    finish_task_switch();
    local_irq_restore(fl);
}

static void schedule(void) {
    schedule_locked(local_irq_save());
}

void sched_tick(void) {
    struct percpu *pc = this_cpu();
    if (pc)
        pc->ticks++;               /* cap-3: per-CPU timer-tick count (preempt proof) */
    if (!current_thread)
        return;                    /* scheduler not up yet */
    current_thread->run_ticks++;   /* DDR-735: sampled CPU time (this CPU owns it) */
    if (current_thread->quantum > 0)
        current_thread->quantum--;
    if (current_thread->quantum == 0) {
        current_thread->quantum = current_thread->quantum_reset;
        schedule();
    }
}

void yield(void) {
    if (!current_thread)
        return;
    current_thread->quantum = current_thread->quantum_reset;
    schedule();
}

/* Block the current thread until something calls sched_unblock on it. Callers
 * hold a lock (cli) across the condition check + block to avoid lost wakeups. */
void sched_block(void) {
    if (!current_thread)
        return;
    current_thread->state = THREAD_BLOCKED;
    schedule();                    /* switch away; returns when unblocked + run */
}

void sched_block_on(spinlock_t *lk) {
    /* DDR-SMP-3c-locks-4: BLOCKED is published *under* `lk` so a waker
     * serialized after the unlock always observes it (its CAS can't be lost).
     * If the waker fires before schedule(), it CASes us READY and schedule()
     * just re-queues us — a benign spurious wake the caller's while() re-checks.
     * IRQs are left as the caller set them (masked, via spin_lock_irqsave);
     * context_switch preserves RFLAGS so we resume masked, matching the old
     * cli/sti path. */
    if (!current_thread) {
        spin_unlock(lk);
        return;
    }
    current_thread->state = THREAD_BLOCKED;
    spin_unlock(lk);
    schedule();
    spin_lock(lk);
}

void sched_unblock(struct tcb *t) {
    /* DDR-SMP-3c-locks-1: BLOCKED->READY is a pure state transition (no ring
     * topology), made an atomic CAS so an AP job may wake a BSP thread; the
     * BSP's locked walk observes READY this pass or the next tick (benign). */
    if (!t)
        return;
    uint32_t expected = THREAD_BLOCKED;
    if (__atomic_compare_exchange_n(&t->state, &expected, THREAD_READY,
                                    0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        /* rq-1: a freshly READY thread must be findable — enqueue it on the
         * waker's own CPU's FIFO. rq_push is a leaf lock, safe from IRQ handlers
         * and under device compl locks. */
        struct percpu *pc = this_cpu();
        int self = pc ? (int)pc->cpu_idx : 0;
        rq_push(self, t);
        /* rq-3: kick an idle CPU so it steals this thread NOW rather than on its
         * next 10 ms tick. Directed IPI to the first idling non-self CPU; the
         * timer remains the backstop if none is (visibly) idle. */
        for (int c = 0; c < PERCPU_MAX; c++) {
            struct percpu *o = percpu_get((uint32_t)c);
            if (c != self && o && o->present && o->idle) {
                smp_resched_one((uint32_t)c);
                break;
            }
        }
    }
}

/* Terminate the current thread: save its exit status, become a ZOMBIE (so the
 * parent's wait4 or the reaper can collect it), wake any waiter, and switch away
 * for good. The address space + TCB are reclaimed by the collector, never here —
 * we are still executing on this thread's kernel stack. */
void sched_exit(int status) {
    /* 5d: PID 1 must never exit. If init returns or _exit()s, halt loudly —
     * the system has no reaper / no first process to fall back on. */
    if (g_init_pid && current_thread->pid == g_init_pid) {
        __asm__ volatile("cli");
        kputs("[panic] init exited — system halted\r\n");
        for (;;)
            __asm__ volatile("hlt");
    }
    /* DDR-729: reclaim any Layer-7 surfaces this process owns before it dies, so a
     * client that exits without SYS_SURFACE_CLOSE never leaks its slot + frames.
     * surface_reap_pid takes its own g_surf_lock (no ordering vs. the ring lock,
     * which we have not taken yet). No-op for kernel threads (no owned surfaces). */
    void surface_reap_pid(uint32_t pid);   /* kernel/syscall/sys_surface.c (DDR-729) */
    void socket_reap_pid(uint32_t pid);    /* kernel/syscall/sys_socket.c (DDR-731) */
    /* DDR-735: capture the final CPU counters into the agent roster slot (if this
     * pid holds one) so the metrics stay readable post-mortem without depending on
     * a SYS_AGENT_METRICS read having landed during the agent's life. */
    void agent_metrics_reap(uint32_t pid, uint64_t run_ticks, uint64_t dispatches);
    if (current_thread->is_user) {
        surface_reap_pid(current_thread->pid);
        socket_reap_pid(current_thread->pid);
        agent_metrics_reap(current_thread->pid,
                           current_thread->run_ticks, current_thread->dispatches);
    }
    /* DDR-SMP-exit-stack-race, rq-2 edition. The dying thread still executes on
     * its kernel stack until context_switch completes, so a collector (wait4 /
     * reaper, possibly on another CPU) must not free that stack before then.
     * rq-1 achieved this by holding g_sched_lock across the final switch; rq-2
     * removes the lock from the switch and uses the on_cpu handshake instead:
     * `on_cpu` STAYS set here, sched_free_tcb spin-waits on it, and the thread
     * that resumes on this CPU release-stores it in finish_task_switch — which
     * happens strictly after the switch has left this stack. The guarantee is
     * unchanged; the mechanism is now local rather than global.
     * The lock still covers the reparent ring walk (topology, D5). */
    uint64_t fl = irq_save();
    /* 5d: reparent this thread's children to init so PID 1 reaps the whole
     * subtree (live children become orphans on this exit; zombies too). */
    if (g_init_pid) {
        struct tcb *t = current_thread->next;
        while (t != current_thread) {
            if (t->parent_pid == current_thread->pid)
                t->parent_pid = g_init_pid;
            t = t->next;
        }
    }
    current_thread->exit_status = status;
    current_thread->state = THREAD_ZOMBIE;      /* NOT pickable; on_cpu stays set */
    if (current_thread->waiter)
        sched_unblock(current_thread->waiter);
    irq_restore(fl);                            /* topology lock only */
    schedule();                                 /* never returns */
    for (;;)                   /* unreachable */
        __asm__ volatile("hlt");
}

/* DDR-837: the live thread with this pid, or NULL. Zombies and finished kernel
 * threads are NOT returned — checkpointing something already dead should say
 * -ESRCH rather than quietly succeed against a corpse. */
struct tcb *sched_find_pid(uint32_t pid) {
    if (pid == 0)
        return 0;
    struct tcb *t = current_thread;
    do {
        if (t->pid == pid && t->state != THREAD_ZOMBIE && t->state != THREAD_DONE)
            return t;
        t = t->next;
    } while (t != current_thread);
    return 0;
}

/* 1 if a runnable/blocked thread with this pid exists (a "living parent"). */
static int pid_alive(uint32_t pid) {
    if (pid == 0)
        return 0;              /* parent_pid 0 == kernel/none -> treat as orphan */
    struct tcb *t = current_thread;
    do {
        if (t->pid == pid && t->state != THREAD_ZOMBIE && t->state != THREAD_DONE)
            return 1;
        t = t->next;
    } while (t != current_thread);
    return 0;
}

/* Low-priority kernel thread: reap orphaned zombies (exited user procs whose
 * parent is gone and which no wait4 is collecting), bounding the zombie leak. A
 * child still being waited on (waiter != 0) or with a living parent is left for
 * its parent's wait4. Reaps at most one per pass under interrupts-off, then
 * yields. */
static void reaper_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t fl = irq_save();
        struct tcb *victim = 0;
        struct tcb *t = current_thread->next;
        while (t != current_thread) {
            if (t->state == THREAD_ZOMBIE && !t->waiter && !pid_alive(t->parent_pid)) {
                victim = t;
                break;
            }
            t = t->next;
        }
        uint64_t cr3 = 0;
        if (victim) {
            cr3 = victim->cr3;
            sched_ring_unlink(victim);     /* claim + unlink atomically under the lock */
        }
        irq_restore(fl);
        if (victim) {                      /* free outside the lock (cap-2a D2) */
            sched_free_tcb(victim);
            if (cr3)
                vmm_destroy_address_space(cr3);
        }
        yield();
    }
}

void sched_start_reaper(void) {
    sched_create(reaper_thread, 0, "reaper");
}
