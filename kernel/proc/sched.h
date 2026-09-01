/* kernel/sched.h — NEXUS round-robin scheduler + thread control block (2c).
 *
 * Minimal preemptive multitasking: a circular ready ring of kernel threads,
 * each with its own stack, switched by the PIT tick when its quantum expires.
 * The 3-lane adaptive scheduler (NAS) from the Layer-2 board layers on later.
 */
#pragma once
#include <stdint.h>
#include "cap.h"
#include "fd.h"
#include "regs.h"
#include "spinlock.h"

typedef void (*thread_fn)(void *);

/* Default per-thread filesystem write budget (bytes). Caps how much a single
 * thread can write through the VFS so a buggy/hostile consumer cannot exhaust
 * the block device. Generous for tools, far below any real disk. */
/* ADR-032: token-bucket rate limit (replaced ADR-015's lifetime cap). The bucket
 * caps a burst / idle accumulation; it refills per timer tick, so a thread's
 * LIFETIME writes are unbounded but its RATE is capped (anti-flood). */
#define FS_WRITE_BURST_MAX       (1u << 20)  /* 1 MiB bucket cap */
#define FS_WRITE_REFILL_PER_TICK (256u << 10) /* 256 KiB/tick @100Hz = 25 MiB/s   */
#define FS_WRITE_BUDGET_DEFAULT  FS_WRITE_BURST_MAX  /* initial full bucket */
#define SPAWN_DEPTH_MAX          3u  /* DDR-838: deepest agent-lineage generation */

enum thread_state {
    THREAD_READY = 0,
    THREAD_RUNNING = 1,
    THREAD_DONE = 2,           /* finished kernel thread (no AS to reclaim)     */
    THREAD_BLOCKED = 3,
    THREAD_ZOMBIE = 4          /* exited user proc awaiting wait4/reaper (5b-9) */
};

/* Anonymous mmap region tracking (5b, ADR-022). A fixed table per process;
 * npages == 0 marks a free slot. Lets sys_munmap find + free a region and
 * (later) a reaper reclaim everything an exited process mapped. */
#define VM_AREA_MAX 64
struct vm_area {
    uint64_t base;     /* page-aligned start in the user mmap arena */
    uint64_t npages;   /* length in pages; 0 = free slot            */
};

/* DDR-872: 4096 covers x87+SSE (512), AVX (576), and the full AVX-512
 * component set (~2688) with margin. Asserted against CPUID.0xD at boot. */
#define FPU_STATE_MAX 4096u

struct tcb {
    uint64_t   rsp;            /* saved stack pointer (offset 0; asm relies on it) */
    uint64_t   kstack_base;    /* base of the thread's kernel stack               */
    uint32_t   tid;
    uint32_t   state;
    uint32_t   quantum;        /* ticks left in the current slice                 */
    uint32_t   quantum_reset;  /* slice length in ticks                           */
    struct tcb *next;          /* circular ready ring                             */
    thread_fn  entry;
    void      *arg;
    const char *name;
    char       name_buf[16];   /* DDR-756: SYS_SETNAME target; name points here after rename */
    struct cap_table *caps;    /* per-thread (process) capability table */
    uint64_t   fs_write_budget; /* ADR-032: token-bucket balance (bytes)   */
    uint64_t   fs_budget_tick;  /* ADR-032: g_ticks at last budget refill  */

    /* Ring-3 (user) thread metadata — zero/unused for pure kernel threads. */
    uint32_t   is_user;
    uint32_t   pid;
    uint64_t   user_rip;       /* user-mode entry virtual address */
    uint64_t   user_stack;     /* user-mode stack top (virtual)   */
    uint64_t   user_arg;       /* value delivered to the user in RDI */
    uint64_t   cr3;            /* process page-table root; 0 == kernel master AS */

    struct fd_table fdt;       /* per-process file descriptors (5b, ADR-022) */
    int        root_mnt;       /* mount paths resolve against (-1 = none)    */
    cap_t      fs_cap;         /* FS capability granted at load (5b)         */

    struct vm_area vma[VM_AREA_MAX];  /* anonymous mmap regions (5b)         */
    uint64_t   mmap_next;      /* bump pointer for addr==NULL mmaps           */

    /* Process model (5b slice 8+). Appended at struct end so the ~30 includers
     * of sched.h keep their field offsets. */
    uint32_t   parent_pid;     /* creating process pid; 0 for kernel threads  */
    int64_t    fork_retval;    /* child's fork() return (0); -1 when unset     */
    int        exit_status;    /* set by sched_exit; collected by wait4 (5b-9) */
    struct tcb *waiter;        /* parent blocked in wait4 on this thread, or 0 */

    /* POSIX signals (PROC-C). */
    uint64_t   sig_pending;      /* bitmap of pending signal numbers          */
    uint64_t   sig_handlers[32]; /* ring-3 handler VA per signal (NSIG=32)    */
    struct regs sig_saved;       /* register snapshot restored by sigreturn   */
    int        sig_active;       /* 1 while a signal handler is running        */

    /* Thread pointer (PROC-D, ADR-023). FS-base MSR value for this thread; set by
     * SYS_SET_TLS, restored into IA32_FS_BASE on every switch to this user thread.
     * 0 for a fresh thread / pure kernel threads. */
    uint64_t   fs_base;

    /* Eager per-thread x87+SSE state (5d, ADR-023 §D8). FXSAVE area, 16-aligned
     * (FXSAVE/FXRSTOR #GP otherwise; the TCB is kmalloc'd ->>=16-aligned, and the
     * attribute keeps the field on a 16-byte boundary). Saved on switch-out,
     * restored on switch-in, so concurrent FPU users never clobber each other. */
    /* DDR-872 (item 18): sized and aligned for XSAVE, not just FXSAVE.
     * XSAVE needs 64-byte alignment (FXSAVE only 16), and the AVX-512
     * state components push the area well past 512 bytes — ~2688 on a
     * Skylake-X class CPU. FPU_STATE_MAX is a compile-time ceiling that
     * boot ASSERTS against the CPU's reported size and panics if it does
     * not fit, rather than truncating: a short XSAVE area is not a
     * smaller save, it is memory corruption past the end of the tcb. */
    uint8_t    fpu_state[FPU_STATE_MAX] __attribute__((aligned(64)));

    /* Full-register fork (5e). A forked child resumes via signal_sigreturn with
     * the parent's complete register frame (RAX=0) instead of enter_user_mode
     * (which zeroes the GP regs) — so non-inlined code in the child keeps a valid
     * RBP/RBX/R12-R15. `forked` selects the launch path in user_launch. */
    int        forked;
    struct regs fork_regs;

    /* AETHER agent governance (Layer 6, ADR-026). Appended at struct end so the
     * ~30 includers of sched.h keep their field offsets. Zero for non-agents
     * (kmalloc'd TCB is zeroed), so a 0 mem_limit lazily means 128 MiB. */
    uint32_t   is_agent;        /* 1 = CAP_AGENT process: rate-limited + mem-capped */
    uint32_t   agent_caps;      /* DDR-982: per-agent action authority — a mask of
                                 * CAP_OCR/CAP_EXEC/CAP_SCENE/CAP_NET_BROWSE.
                                 * Follows the is_agent/is_net pattern (authority
                                 * held BY the process) rather than the NCS handle
                                 * pattern, because the ring-3 submit ABI is
                                 * (type,payload,len) and carries no handle to
                                 * check. Minted before the agent's first run.
                                 * NON-NEGOTIABLE 10: kmalloc does not zero, so
                                 * sched_create initialises this explicitly. */
    uint32_t   is_sovereign;    /* 1 = CAP_SOVEREIGN (operator/daemon): mode+approve */
    uint64_t   mem_limit;       /* hard cap in bytes; 0 -> AETHER_MEM_DEFAULT       */
    uint64_t   mem_used;        /* charged by mmap growth; uncharged on munmap      */
    uint32_t   sc_count;        /* syscalls issued in the current rate window       */
    uint64_t   sc_window_start; /* g_ticks at the window's open                     */

    /* SMP scheduling (ADR-031 / DDR-SMP-3c-cap-2a). Which CPU is running this
     * thread (-1 = none): schedule() picks only READY threads with on_cpu<0, so
     * two CPUs can never run one thread. Appended at struct end (offset stability). */
    int        on_cpu;
    uint32_t   is_idle;         /* cap-2b: per-CPU idle thread; never cross-CPU picked */
    struct tcb *rq_next;        /* rq-1: intrusive per-CPU ready-FIFO link (NULL = tail
                                 * or not enqueued; rq membership tracked by rq_on) */
    int        rq_on;           /* rq-1: 1 while linked into some CPU's ready queue */
    /* ---- DDR-895 (item 16 DIAGNOSTIC): observation only ------------------
     * These fields are WRITTEN but never read by any scheduling decision. The
     * pick stays FIFO. That is deliberate: measuring a candidate algorithm by
     * running it changes the timing you are trying to measure, and DDR-894's
     * revert exists because a scheduler change altered boot-phase progress in
     * ways its own gate could not see. */
    uint64_t   dbg_vruntime;    /* what vruntime WOULD be, if it were used     */
    uint64_t   dbg_v_at_create; /* floor at creation — H2's signature          */
    uint64_t   dbg_v_at_wake;   /* floor at last unblock — H2's signature      */
    uint32_t   dbg_picks;       /* times chosen by the real (FIFO) picker      */
    uint32_t   dbg_ticks;       /* ticks charged while running                 */
    uint32_t   weight;
    uint64_t   vt_in;
    uint32_t   vt_cpu;         /* DDR-989 §8.4: cpu that STAMPED vt_in. Tests the
                                * cross-CPU-TSC hypothesis; init in sched_create
                                * (§NON-NEGOTIABLE 10 — kmalloc does not zero).  */
    uint32_t   dbg_yields;
    uint32_t   sched_woke;
    uint32_t   dbg_ebadf_seen;  /* DDR-946: first -EBADF write already reported */
    uint32_t   dbg_writes;      /* DDR-948: sys_write+sys_writev ATTEMPTS by this thread */
    struct tcb *blk_wait_next;  /* DDR-878: intrusive FIFO link for virtio-blk's
                                 * slot wait list. Separate from rq_next because a
                                 * thread waiting for a request slot is BLOCKED and
                                 * simultaneously NOT on any ready queue — reusing
                                 * rq_next would splice the two lists together. */
    uint32_t   is_net;          /* DDR-731: CAP_NET — may open proxy sockets (SYS_SOCK_*).
                                 * Granted at agent spawn; NOT inherited across fork. */
    uint32_t   is_memory;       /* DDR-836: CAP_MEMORY — agent memory store (NSI 82/83).
                                 * Granted at spawn; NOT inherited across fork. */
    uint32_t   agent_depth;     /* DDR-838: generations below the founding agent. 0 fo
                                 * processes outside any agent lineage. Keyed on
                                 * LINEAGE, not on is_agent — fork does not inherit
                                 * authority flags, so a cap on is_agent would be
                                 * escaped by one fork. */
    uint32_t   is_ipc;          /* DDR-1033: may use the ring-3 IPC door (NSI 98/99).
                                 * Kernel-set at spawn, never mintable by the
                                 * process. Zeroed in sched_create: kmalloc does
                                 * not zero (§NON-NEGOTIABLE 10). */
    cap_t      ipc_cap;         /* DDR-1033: the RES_IPC handle minted beside it */
    uint32_t   is_exec;         /* DDR-1034: may use the bounded experiment
                                 * executor (NSI 100/101). Kernel-set at spawn,
                                 * never mintable by the process -- the same
                                 * shape as is_ipc. Zeroed in sched_create:
                                 * kmalloc does not zero (NON-NEGOTIABLE 10). */
    cap_t      exec_cap;        /* DDR-1034: the RES_EXEC handle minted beside it */
    uint32_t   is_rewrite;      /* DDR-842: CAP_REWRITE — code-rewrite approval.
                                 * Meaningless without is_sovereign; the syscall
                                 * requires BOTH. Granted at spawn only. */
    uint32_t   checkpointed;    /* DDR-837: operator froze this agent. The thread
                                 * blocks ITSELF on seeing this at its next syscall
                                 * boundary — never set THREAD_BLOCKED from anothe
                                 * CPU, which races the scheduler. */
    /* DDR-735: CPU accounting, written ONLY by the owning CPU (sched_tick on the
     * running CPU; schedule() under the on_cpu claim) — lock-free by exclusion. */
    uint64_t   run_ticks;       /* 100 Hz ticks observed while current (sampled CPU time) */
    uint64_t   dispatches;      /* times the scheduler switched this thread in           */
    /* DDR-955: bounded-wait fields */
    uint64_t   block_deadline;   /* g_ticks + timeout; 0 = no deadline */
    int        wake_timed_out;   /* 1 if timer expired before wakeup   */
};

/* DDR-743: read-only process snapshot for SYS_GETPROCS (ring-3 `ps`). Pure
 * value copy (no pointers), so a caller can copyout it directly. */
struct procinfo {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;             /* raw THREAD_* enum */
    uint32_t flags;             /* bit 0 = is_user */
    char     name[16];
    uint64_t run_ticks;         /* DDR-754: 100 Hz ticks observed while current */
    uint64_t dispatches;        /* DDR-754: scheduler switch-in count           */
};
/* Fill *out with the index-th thread in the scheduler ring (walked under
 * g_sched_lock). Returns 1 if filled, 0 if index is past the last thread. */
int         sched_snapshot(int index, struct procinfo *out);
/* DDR-1001: wait4's child lookup, walking the all-threads ring UNDER
 * g_sched_lock. sys_wait.c cannot take that lock (it is file-local to sched.c),
 * which is why the walk lives here rather than there. */
struct tcb *sched_find_child(struct tcb *parent, int pid,
                             struct tcb **has_live, int *any, int *ringbad);

void        sched_init(void);                                   /* boot ctx -> idle thread */
struct tcb *sched_create(thread_fn entry, void *arg, const char *name);
/* DDR-964: create a kernel thread BLOCKED, so the caller can finish setting
 * fields the entry point reads (notably ->arg) before any CPU can run it.
 * sched_create() is already runnable on return and races rq_steal. The caller
 * MUST sched_unblock() the returned thread. */
struct tcb *sched_create_blocked(thread_fn entry, void *arg, const char *name);
struct tcb *sched_create_user(const char *name, uint64_t user_rip, uint64_t user_stack);
/* fork (5b slice 8): clone `parent` into a ready ring-3 child whose AS is
 * `child_cr3`, resuming at `entry`/`user_rsp` (RAX=0 via enter_user_mode). Copies
 * the cap table (cap_fork) and fd table (fd_clone). Returns the child, or 0 on
 * OOM (any partial child is destroyed). */
struct tcb *sched_create_user_clone(struct tcb *parent, uint64_t child_cr3,
                                    uint64_t entry, uint64_t user_rsp);
/* Unlink a never-run / reaped thread from the ready ring and free its kstack,
 * cap table, open files, and TCB. Not for the currently running thread. */
void        sched_destroy(struct tcb *t);
int         sched_rq_walk_ok(void);   /* DDR-996 §5 arm B: runqueue links sane? */
/* DDR-989 §4: vruntime + pick-count for the running thread and the queue head. */
void        sched_vr_sample(uint32_t *cur_pid, uint64_t *cur_vr, uint32_t *cur_pk,
                            uint32_t *head_pid, uint64_t *head_vr, uint32_t *head_pk);
int         sched_rqfree_probe(int n); /* DDR-996 §5 arm A: force the window   */
void        sched_tick(void);                                   /* from the timer IRQ      */
void        yield(void);                                        /* cooperative switch      */
void        sched_ap_enter(void);                               /* cap-2b: AP joins the scheduler (never returns) */
extern volatile int g_user_on_ap;                               /* cap-4: an AP ran a ring-3 thread (proof flag) */
void        sched_block(void);                                  /* block current; switch away */
/* DDR-SMP-3c-locks-4: sleep on a condition verified under `lk`. Sets the caller
 * BLOCKED *while `lk` is held*, releases `lk`, switches away, and re-takes `lk`
 * on return — so a waker serialized after the release always sees BLOCKED and
 * its sched_unblock CAS cannot be lost. IRQs stay as the caller left them. */
/* DDR-994: report a wait that has gone on too long. Fires ONCE per wait (the
 * `noted` flag), then the caller keeps spinning — this REPORTS, it does not
 * repair. Thresholds below; both must be crossed. */
#define YIELD_STALL_SPINS 20000u  /* enough spins to mean "not just contended" */
#define YIELD_STALL_TICKS 500u    /* ...AND 5 s at 100 Hz. Neither alone suffices. */    /* ...AND 5 s at 100 Hz. Neither alone suffices. */
void        yield_stall_note(const char *site, uint32_t spins, uint64_t ticks, int *noted);
void        yield_stall_done(const char *site, uint32_t spins, uint64_t ticks, int *noted);

void        sched_block_on(spinlock_t *lk);
int         sched_block_timeout(spinlock_t *lk, volatile int *done,
                                uint64_t timeout_ticks);
/* DDR-885 (item 37): how many steals stayed on-node vs crossed. Diagnostic —
 * the gate reads these to show the two-pass order is actually exercised. */
void        sched_steal_counts(uint64_t *local, uint64_t *remote);

/* DDR-895 (item 16 diagnostic). Dump the selection trace and per-thread
 * accounting. Probe-gated; never called on a default boot. */
void        sched_trace_dump(void);

void        sched_unblock(struct tcb *t);                       /* mark a blocked thread ready */
struct tcb *sched_find_pid(uint32_t pid);                       /* DDR-837: live thread by pid, or NULL */
void        sched_exit(int status);                             /* zombie + status; wakes waiter */
void        sched_start_reaper(void);                           /* spawn the orphan-zombie reaper */
void        sched_set_init_pid(uint32_t pid);                   /* 5d: designate PID 1 (init) */

/* DDR-SMP-3b: the running thread is per-CPU state, read through %gs (valid
 * from kmain top via percpu_init_early). Every existing site — reads and the
 * scheduler's writes — resolves per-CPU unchanged. */
#include "percpu.h"
#define current_thread (this_cpu()->current)
