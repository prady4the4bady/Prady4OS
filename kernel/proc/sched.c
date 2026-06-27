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

struct tcb *current_thread;       /* NULL until sched_init (safe: sched_tick checks) */
static struct tcb idle_tcb;
static uint32_t next_tid = 1;
static uint32_t g_init_pid = 0;   /* 5d: PID 1; orphans reparent here, exit panics */

void sched_set_init_pid(uint32_t pid) { g_init_pid = pid; }

/* First code a freshly-created thread runs (entered via context_switch's RET).
 * current_thread is already the new thread (set by schedule before switching). */
static void thread_trampoline(void) {
    current_thread->entry(current_thread->arg);
    current_thread->state = THREAD_DONE;
    for (;;)
        __asm__ volatile("hlt");
}

void sched_init(void) {
    idle_tcb.tid = 0;
    idle_tcb.name = "idle";
    idle_tcb.state = THREAD_RUNNING;
    idle_tcb.quantum = idle_tcb.quantum_reset = QUANTUM;
    idle_tcb.next = &idle_tcb;     /* ring of one */
    idle_tcb.caps = cap_table_create();

    /* Capture a clean FPU template (SSE already enabled by cpu_enable_sse in
     * kmain, which runs before sched_init). New threads + idle copy this so the
     * first FXRSTOR loads a valid state, not zeros. */
    uint32_t mxcsr = 0x1F80u;          /* round-nearest, all exceptions masked */
    __asm__ volatile("fninit");
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    __asm__ volatile("fxsave (%0)" :: "r"(fpu_init_template) : "memory");
    memcpy(idle_tcb.fpu_state, fpu_init_template, sizeof idle_tcb.fpu_state);

    current_thread = &idle_tcb;
}

struct tcb *sched_create(thread_fn entry, void *arg, const char *name) {
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
    t->state = THREAD_READY;
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

    /* Insert into the ring after the current thread. */
    t->next = current_thread->next;
    current_thread->next = t;
    return t;
}

/* Kernel-side launch for a ring-3 thread: set the ring-0 stack the CPU will use
 * for this thread's syscalls/interrupts, then drop to user mode (never returns). */
static void user_launch(void *arg) {
    (void)arg;
    struct tcb *t = current_thread;
    uint64_t ktop = t->kstack_base + STACK_SIZE;
    tss_set_rsp0(ktop);
    syscall_kstack_top = ktop;
    if (t->forked)
        signal_sigreturn(&t->fork_regs);   /* resume with the parent's full reg set, RAX=0 */
    enter_user_mode(t->user_rip, t->user_stack, t->user_arg);
}

struct tcb *sched_create_user(const char *name, uint64_t user_rip, uint64_t user_stack) {
    struct tcb *t = sched_create(user_launch, 0, name);
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
    /* Mask interrupts across creation + field init: sched_create enqueues a READY
     * thread immediately, and until cr3/user_rip are set a timer tick could run
     * it with a kernel-master cr3 / null entry. (Mirrors the elf_load guard.) */
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");

    struct tcb *t = sched_create(user_launch, 0, parent->name);
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

    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
    return t;
}

void sched_destroy(struct tcb *t) {
    if (!t || t == current_thread)
        return;
    /* Unlink from the circular ready ring (t was inserted after some node). */
    struct tcb *p = t->next;
    while (p->next != t)
        p = p->next;
    p->next = t->next;
    /* Free per-process resources: open files, capability table, kstack, TCB. */
    for (int i = 0; i < FD_MAX; i++)
        fd_free(t, i);
    if (t->caps)
        cap_table_destroy(t->caps);
    if (t->kstack_base)
        kfree((void *)(uintptr_t)t->kstack_base);
    kfree(t);
}

static int runnable(const struct tcb *t) {
    return t->state == THREAD_READY || t->state == THREAD_RUNNING;
}

/* schedule() must be atomic against the timer: it is reached both voluntarily
 * (yield/sched_block) and from the timer IRQ (sched_tick). If a voluntary
 * schedule() ran with interrupts enabled (e.g. the yield in a driver's
 * busy-wait), a timer tick could re-enter schedule() mid-context-switch and
 * corrupt thread state. Masking interrupts here makes it non-reentrant; the
 * per-thread IF is preserved across the switch by context_switch (pushfq/popfq)
 * and the save/restore below restores the caller's IF on resume. */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

/* Switch to the next runnable thread in the ring (round-robin, skipping
 * blocked/finished threads). The idle thread is always runnable, so there is
 * always something to run. */
static void schedule(void) {
    uint64_t fl = irq_save();
    struct tcb *prev = current_thread;

    struct tcb *next = prev->next;
    while (next != prev && !runnable(next))
        next = next->next;

    if (next == prev) {
        if (runnable(prev)) {
            irq_restore(fl);
            return;                /* prev is the only runnable thread */
        }
        next = &idle_tcb;          /* prev blocked and nothing else: fall to idle */
    }

    if (prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    current_thread = next;
    if (next->is_user) {       /* point the CPU at this thread's ring-0 stack */
        uint64_t ktop = next->kstack_base + STACK_SIZE;
        tss_set_rsp0(ktop);
        syscall_kstack_top = ktop;
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

void sched_unblock(struct tcb *t) {
    if (t && t->state == THREAD_BLOCKED)
        t->state = THREAD_READY;
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
    if (current_thread->waiter)
        sched_unblock(current_thread->waiter);
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
        if (victim) {
            uint64_t cr3 = victim->cr3;
            sched_destroy(victim);
            if (cr3)
                vmm_destroy_address_space(cr3);
        }
        irq_restore(fl);
        yield();
    }
}

void sched_start_reaper(void) {
    sched_create(reaper_thread, 0, "reaper");
}
