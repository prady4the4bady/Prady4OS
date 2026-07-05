/* kernel/sched.c — preemptive round-robin kernel scheduler (Phase 2c/2e). */
#include "sched.h"
#include "kheap.h"
#include "console.h"
#include "tss.h"
#include "syscall.h"
#include "vmm.h"
#include "string.h"            /* memcpy for the per-thread FPU template (5d) */
#include "cpu_mitigations.h"   /* cpu_wrmsr + MSR_IA32_FS_BASE (PROC-D) */

#define STACK_SIZE   16384u
#define QUANTUM      2u           /* ticks per slice (PIT @100Hz -> 20 ms) */

/* Clean x87+SSE state (FNINIT + default MXCSR) captured once in sched_init and
 * copied into every new thread's fpu_state (5d, ADR-023 §D8). A zeroed FXSAVE
 * area is NOT clean — it would load MXCSR=0 (all SSE exceptions unmasked). */
static uint8_t fpu_init_template[512] __attribute__((aligned(16)));

static inline void fpu_save(void *area) {
    __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
}
static inline void fpu_restore(const void *area) {
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

static void schedule(void);        /* fwd decl: thread_trampoline schedules on exit */

/* First code a freshly-created thread runs (entered via context_switch's RET).
 * current_thread is already the new thread (set by schedule before switching). */
static void thread_trampoline(void) {
    /* DDR-SMP-3c-locks-1: schedule() switched to this brand-new thread while
     * holding g_sched_lock; a resumed thread releases it in irq_restore, but a
     * first entry lands here instead — release before running the body (the
     * crafted initial RFLAGS already restored IF via context_switch's popfq). */
    spin_unlock(&g_sched_lock);
    current_thread->entry(current_thread->arg);
    /* cap-2b D3: cooperative exit. A returning kernel thread marks itself DONE
     * and schedules away — DONE is unpickable so it never runs again. The old
     * for(;;)hlt relied on timer preemption to leave, which an un-preempted AP
     * (cap-3 pending) never gets, wedging that CPU. */
    current_thread->state = THREAD_DONE;
    current_thread->on_cpu = -1;
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
    __asm__ volatile("fxsave (%0)" :: "r"(fpu_init_template) : "memory");

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
        __asm__ volatile("sti; hlt");  /* sleep until wake IPI / timer */
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
    t->quantum = t->quantum_reset = QUANTUM;
    t->entry = entry;
    t->arg = arg;
    t->name = name;
    t->caps = cap_table_create();
    t->fs_write_budget = FS_WRITE_BUDGET_DEFAULT;
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
    /* The cli guard stays: this reads the GLOBAL syscall_user_* register
     * snapshot, which a local preemption + another syscall would overwrite.
     * cap-2a D3 ADDS BLOCKED-create on top — the child is not pickable by ANY
     * CPU until fully built and sched_unblock'd below (the cli only masks the
     * local CPU). (Per-CPU syscall entry state is a cap-4 concern.) */
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");

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
        memcpy(t->fpu_state, parent->fpu_state, sizeof t->fpu_state);  /* 5d: inherit FPU */
        /* 5e: full-register fork. Resume the child with the parent's complete
         * register frame (callee-saved snapshot from syscall entry) and RAX=0,
         * via signal_sigreturn — so non-inlined code keeps a valid RBP/RBX/R12-15.
         * Caller-saved regs (RDI/RSI/RDX/RCX/R8-R11) are 0: per the syscall ABI the
         * parent's post-`syscall` code does not rely on them. */
        t->forked = 1;
        struct regs *fr = &t->fork_regs;
        memset(fr, 0, sizeof *fr);
        fr->rbx = syscall_user_rbx;  fr->rbp = syscall_user_rbp;
        fr->r12 = syscall_user_r12;  fr->r13 = syscall_user_r13;
        fr->r14 = syscall_user_r14;  fr->r15 = syscall_user_r15;
        fr->rax = 0;                              /* fork returns 0 in the child */
        fr->rip = entry;                          /* = syscall_user_rip          */
        fr->rsp = user_rsp;                       /* = syscall_user_rsp          */
        fr->rflags = syscall_user_rflags;         /* parent's flags (IF set)     */
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
    for (int i = 0; i < FD_MAX; i++)
        fd_free(t, i);
    if (t->caps)
        cap_table_destroy(t->caps);
    if (t->kstack_base)
        kfree((void *)(uintptr_t)t->kstack_base);
    kfree(t);
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

/* cap-2a D1 / cap-2b D1,D4: a thread CPU `cpu` (is_bsp) can pick up. READY and
 * unclaimed (on_cpu<0); a per-CPU idle only by ITS OWN cpu (never cross-picked,
 * but the owner must be able to round-robin back to it — the idle is also that
 * CPU's main context, e.g. the BSP idle runs sched_demo); and a USER thread only
 * on the BSP — ring-3 on an AP needs per-CPU SYSCALL entry state (cap-4). On the
 * BSP with no APs this is exactly "READY, not prev" — the pick set is unchanged. */
static int pickable(const struct tcb *t, int cpu, int is_bsp) {
    if (t->state != THREAD_READY || t->on_cpu >= 0)
        return 0;
    if (t->is_idle && t != g_idle[cpu])
        return 0;
    if (t->is_user && !is_bsp)
        return 0;
    return 1;
}

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
 * always something to run. */
static void schedule(void) {
    uint64_t fl = irq_save();
    struct tcb *prev = current_thread;
    struct percpu *pc = this_cpu();
    int cpu = pc ? (int)pc->cpu_idx : 0;
    int is_bsp = pc ? pc->is_bsp : 1;

    struct tcb *next = prev->next;
    while (next != prev && !pickable(next, cpu, is_bsp))
        next = next->next;

    if (next == prev) {
        if (runnable(prev)) {
            irq_restore(fl);
            return;                /* prev is the only runnable thread; keep it */
        }
        next = g_idle[cpu];        /* prev blocked and nothing else: this CPU's idle */
    }

    /* Release prev (back to the pool for any CPU) and claim next for this CPU.
     * Both under g_sched_lock, so the claim is atomic — no two CPUs take one
     * thread (cap-2a D1). on_cpu clears whenever we switch AWAY from prev — not
     * just when it was RUNNING: a thread that blocked (sched_block set it
     * BLOCKED before calling us) must also release its CPU, or after unblock
     * (BLOCKED->READY) it would fail the on_cpu<0 pick test and never run. */
    if (prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;
    prev->on_cpu = -1;
    next->on_cpu = cpu;
    next->state = THREAD_RUNNING;
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
    /* 5d (ADR-023 §D8): eager per-thread FPU save/restore. Save the outgoing
     * thread's x87+SSE state and load the incoming thread's, so concurrent FPU
     * users (e.g. two ring-3 C processes) never see each other's XMM/x87 regs.
     * Nothing between here and context_switch's return into `next` touches the
     * FPU (this path is -mgeneral-regs-only). */
    fpu_save(prev->fpu_state);
    fpu_restore(next->fpu_state);
    context_switch(&prev->rsp, next->rsp);
    /* resumed here later, as `prev`, when scheduled again */
    irq_restore(fl);
}

void sched_tick(void) {
    if (!current_thread)
        return;                    /* scheduler not up yet */
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
    __atomic_compare_exchange_n(&t->state, &expected, THREAD_READY,
                                0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
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
    /* cap-2a D2: the reparent ring walk + the ZOMBIE transition are topology —
     * take g_sched_lock so a concurrent CPU's walk can't observe a torn ring.
     * Released before schedule() (which re-takes it); a ZOMBIE is not pickable,
     * so the gap is safe. */
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
    current_thread->state = THREAD_ZOMBIE;
    current_thread->on_cpu = -1;               /* no longer occupies this CPU */
    struct tcb *waiter = current_thread->waiter;
    irq_restore(fl);
    if (waiter)
        sched_unblock(waiter);
    schedule();
    for (;;)                   /* unreachable */
        __asm__ volatile("hlt");
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
