/* kernel/syscall/syscall.c — NSI dispatch table, handlers, and MSR setup. */
#include "syscall.h"
#include "console.h"
#include "lapic.h"    /* DDR-1010: lapic_id() -- GS-independent CPU identity */
#include "cap.h"
#include "sched.h"
#include "uaccess.h"   /* validated user-pointer copy path (ADR-022); used by 5b syscalls */
#include "errno.h"
#include "sys_io.h"    /* SYS_READ / SYS_WRITE handlers (slice 3) */
#include "sys_file.h"  /* SYS_OPEN / SYS_CLOSE / SYS_FSTAT handlers (slice 4) */
#include "sys_proc.h"  /* SYS_LSEEK / SYS_GETCWD handlers (slice 5) */
#include "sys_mmap.h"  /* SYS_MMAP / SYS_MUNMAP handlers (slice 6) */
#include "sys_exec.h"  /* SYS_EXECVE handler (slice 7) */
#include "sys_fork.h"  /* SYS_FORK handler (slice 8) */
#include "sys_wait.h"  /* SYS_WAIT4 handler (slice 9) */
#include "pipe.h"      /* SYS_PIPE / SYS_DUP2 handlers (PROC-A) */
#include "epoll.h"     /* SYS_EPOLL_* handlers (PROC-B) */
#include "signal.h"    /* SYS_SIGACTION / _KILL / _SIGRETURN (PROC-C) */
#include "sys_io_uring.h" /* SYS_IO_URING_* handlers (PROC-E) */
#include "aether.h"       /* AETHER syscalls + per-agent rate limit (Layer 6) */
#include "percpu.h"       /* DDR-SMP-3a: %gs:0 probe from syscall context */

void sys_aether_register(void);   /* kernel/syscall/sys_aether.c */
void sys_socket_register(void);   /* kernel/syscall/sys_socket.c (ADR-027) */
void sys_fb_register(void);       /* kernel/syscall/sys_fb.c (DDR-702) */
void sys_input_register(void);    /* kernel/syscall/sys_input.c (DDR-703) */
void sys_surface_register(void);  /* kernel/syscall/sys_surface.c (DDR-706) */
void sys_acc_register(void);      /* kernel/syscall/sys_acc.c (DDR-813) */
void sys_ags_register(void);      /* kernel/syscall/sys_ags.c (DDR-814) */
void sys_vault_register(void);    /* kernel/syscall/sys_vault.c (DDR-834) */
void sys_agentmem_register(void); /* kernel/syscall/sys_agentmem.c (DDR-836) */
void sys_checkpoint_register(void); /* kernel/syscall/sys_checkpoint.c (DDR-837) */
void sys_rewrite_register(void);  /* kernel/syscall/sys_rewrite.c (DDR-842) */
void sys_experiment_register(void); /* kernel/syscall/sys_experiment.c (DDR-1034) */
void sys_audit_register(void);    /* kernel/syscall/sys_audit.c (DDR-842) */

#define MAX_SYSCALLS 128  /* NSI-v2 table size (ADR-022). Raised 80->128 in the
                           * DDR-823 audit: NSI 77-87 are already sequenced and
                           * 80+ would have registered into the void. 128 is
                           * headroom, not a prediction — syscall_register()
                           * now panics rather than dropping silently, so the
                           * next overflow announces itself at boot. */

static syscall_fn table[MAX_SYSCALLS];
/* syscall_kstack_top moved into the percpu area at [gs:16] (DDR-SMP-3b); the
 * user-register snapshot (rsp/rip/callee-saved/rflags) moved there too at
 * [gs:56..120] — per-CPU, written by syscall_entry.asm (ADR-031 cap-4). */

extern void syscall_entry(void);   /* arch/x86_64/syscall_entry.asm */

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* DDR-823 audit. This used to be a bare `if (num < MAX_SYSCALLS)` — an
 * out-of-range registration was SILENTLY DISCARDED. The syscall then returned
 * -ENOSYS at runtime, so the symptom appeared in a gate, far from the cause,
 * looking like a broken handler rather than a table that was never populated.
 *
 * That matters right now rather than hypothetically: NSI 77-87 are already
 * sequenced (ACC 77/78, AGS 79/80, agent-update 81, memory 82/83, checkpoint
 * 84/85, rewrite-approve 86, read-audit 87). With the old MAX_SYSCALLS of 80,
 * everything from 80 up would have registered into the void.
 *
 * Now it is loud and it is at boot, where a programming error belongs. Same
 * principle as ci-shard-check and ci-start-align-check: a silent drop that
 * looks like success is the failure mode this project keeps paying for. */
void syscall_register(unsigned num, syscall_fn fn) {
    if (num >= MAX_SYSCALLS) {
        kputs("PANIC: syscall_register() past MAX_SYSCALLS — raise it in "
              "kernel/syscall/syscall.c. NSI numbers are append-only, so this "
              "is a table-size bug, never a reason to reuse a number.\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    table[num] = fn;
}

/* DDR-1010 §7: the SWAPGS discipline check, made CONTINUOUS and CPU-identifying.
 *
 * The DDR-SMP-3a probe in sys_getpid below is ONE-SHOT (`static int gs_checked`)
 * and fires on the first sys_getpid of the whole boot. It caught OPEN-2 only
 * because that boot's corruption happened to be early; it cannot bound WHEN GS
 * goes bad, and it prints no CPU index.
 *
 * This runs at the top of every syscall, BEFORE anything dereferences
 * current_thread -- which matters, because the rate-limit check below is itself
 * a `current_thread->is_agent` read, and on the DDR-1010 boot that read went
 * through a GS base of 0 into the real-mode IVT.
 *
 * COST: the always-on part is the two instructions of this_cpu() plus a compare.
 * The expensive identification -- lapic_id() is an MMIO read -- runs ONLY after
 * the cheap check has already failed, so a healthy boot pays nothing for it.
 *
 * percpu_by_apic_id() is the right cross-reference precisely because it does not
 * read %gs (percpu.c:104): it can name the CPU when %gs cannot.
 *
 * Latched per CPU slot so a wedged CPU cannot flood the log; the latch is keyed
 * on the LAPIC id, not on the (unusable) percpu index. */
static uint32_t g_gsfail_seen;                  /* bitmask of apic_ids reported */

static void gs_discipline_check(long num) {
    struct percpu *pc = this_cpu();
    if (pc && pc->self == pc)
        return;                                  /* healthy: the common path */

    uint32_t aid = lapic_id();
    if (aid < 32 && (g_gsfail_seen & (1u << aid)))
        return;                                  /* already reported this CPU */
    if (aid < 32)
        g_gsfail_seen |= 1u << aid;

    struct percpu *real = percpu_by_apic_id(aid);
    kputs("[percpu] gs FAIL (syscall ctx) apic=");
    kputdec(aid);
    kputs(" num=");
    kputdec((uint64_t)num);
    kputs(" gs0=");
    kputhex((uint64_t)(uintptr_t)pc);            /* kputhex emits its own 0x (§INV.9) */
    kputs(" want=");
    kputhex((uint64_t)(uintptr_t)real);
    kputs("\r\n");
}

long syscall_dispatch(long num, long a1, long a2, long a3, long a4,
                      long a5, long a6) {
    if (num < 0 || num >= MAX_SYSCALLS || !table[num])
        return -ENOSYS;
    gs_discipline_check(num);                    /* DDR-1010: before any deref */
    /* AETHER rate limit (ADR-026 D7): agent processes get 60 syscalls / 1 s; an
     * over-budget agent is cleanly killed here and never reaches the handler.
     * Non-agents (init, PRISM, kernel) are exempt, so existing gates are intact.
     *
     * ADR-036 supersedes D7's COUNTING SCOPE (budget, window, kill and log are
     * unchanged): SYS_YIELD does not count. D7 exists to bound "a tight-loop
     * agent's ability to DoS the single core", and SYS_YIELD is the one syscall
     * whose entire semantic is surrendering that core — counting it killed
     * agents for cooperating. A rendezvous costs 2 syscalls per poll, so a
     * correctly-waiting agent died at ~30 iterations no matter how well its
     * peer behaved. The exemption is ENUMERATED, not a category: every other
     * syscall, SYS_MEMORY_READ included, still counts, so an agent spamming
     * real work is killed exactly as before. Widening this list needs an ADR. */
    if (current_thread && current_thread->is_agent && num != SYS_YIELD &&
        aether_rate_check(current_thread) < 0)
        sched_exit(137);                         /* never returns */
    /* DDR-837: an operator checkpoint. The thread blocks ITSELF here, which is
     * already a known-safe boundary (the rate limiter above kills a thread at
     * this same point). Setting THREAD_BLOCKED from the checkpointing CPU would
     * race the scheduler for a target that may be RUNNING elsewhere. Re-checked
     * in a loop because a spurious wake must not resume a frozen agent. */
    while (current_thread && current_thread->checkpointed)
        sched_block();
    return table[num](a1, a2, a3, a4, a5, a6);
}

/* --- handlers ------------------------------------------------------------- */

static long sys_putc(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    cap_t cap = (cap_t)a1;
    /* Mutating op: require a capability bound to the console with display rights. */
    if (!cap_authorize(current_thread->caps, cap, RES_DEVICE, CONSOLE_RES_ID, CAP_DISPLAY))
        return -1;
    kputc((char)a2);
    return 0;
}

static long sys_getpid(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a1; (void)a2; (void)a3; (void)a4;
    /* DDR-SMP-3a probe (once): a %gs:0 read from a syscall entered at ring 3 —
     * this only works when the SWAPGS discipline is balanced. */
    static int gs_checked;
    if (!gs_checked) {
        gs_checked = 1;
        struct percpu *pc = this_cpu();
        if (pc && pc->self == pc)
            kputs("[percpu] gs OK (syscall ctx)\r\n");
        else
            kputs("[percpu] gs FAIL (syscall ctx)\r\n");
        /* DDR-SMP-3b: current_thread resolves through the same percpu slot. */
        if (pc && pc->current && pc->current == current_thread)
            kputs("[percpu] current OK (syscall ctx)\r\n");
        else
            kputs("[percpu] current FAIL (syscall ctx)\r\n");
    }
    return (long)current_thread->pid;
}

static long sys_yield(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a1; (void)a2; (void)a3; (void)a4;
    yield();
    return 0;
}

static long sys_exit(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    /* DDR-940: name the thread. Without the pid this line cannot answer the
     * question it is most often read for — in a smoke-agent-click failure a
     * bare sys_exit(0) right after PRADYOS_AGENT_TRIGGER pid=82 could be the
     * triggered agent exiting before it printed anything (so it DID run), or
     * an unrelated thread (so the agent never ran). Those are opposite
     * conclusions about the same log. */
    kputs("[user] sys_exit(");
    kputdec((uint64_t)a1);
    kputs(") pid=");
    kputdec((uint64_t)current_thread->pid);
    /* DDR-948: write ATTEMPTS by this thread. On an A1 failure (agent exits 0
     * with no AGENT_START and no EBADF), writes=0 means main was never entered
     * — the defect is pre-main in the crt/ELF entry; writes>0 means the writes
     * were attempted and accepted yet produced no serial output, putting the
     * defect downstream of fd_write_user. Two different subsystems. */
    kputs(" writes=");
    kputdec((uint64_t)current_thread->dbg_writes);
    kputs(" — thread terminating\r\n");
    sched_exit((int)a1);       /* zombie w/ status; does not return */
    return 0;
}

void syscall_init(void) {
    for (int i = 0; i < MAX_SYSCALLS; i++)
        table[i] = 0;
    syscall_register(SYS_PUTC, sys_putc);
    syscall_register(SYS_GETPID, sys_getpid);
    syscall_register(SYS_YIELD, sys_yield);
    syscall_register(SYS_EXIT, sys_exit);
    sys_io_register();                   /* SYS_READ / SYS_WRITE (slice 3) */
    sys_file_register();                 /* SYS_OPEN / SYS_CLOSE / SYS_FSTAT (slice 4) */
    sys_proc_register();                 /* SYS_LSEEK / SYS_GETCWD (slice 5) */
    sys_mmap_register();                  /* SYS_MMAP / SYS_MUNMAP (slice 6) */
    sys_exec_register();                  /* SYS_EXECVE (slice 7) */
    sys_fork_register();                  /* SYS_FORK (slice 8) */
    sys_wait_register();                  /* SYS_WAIT4 (slice 9) */
    pipe_register();                      /* SYS_PIPE / SYS_DUP2 (PROC-A) */
    epoll_register();                     /* SYS_EPOLL_* (PROC-B) */
    signal_register();                    /* SYS_SIGACTION / _KILL / _SIGRETURN (PROC-C) */
    sys_io_uring_register();              /* SYS_IO_URING_* (PROC-E) */
    sys_aether_register();                /* SYS_GET_MODE..SYS_SET_MEM_LIMIT (Layer 6) */
    sys_socket_register();                /* SYS_SOCK_* proxy sockets (ADR-027) */
    sys_ags_register();                   /* SYS_GOAL_SIGN / SYS_GOAL_VERIFY (DDR-814) */
    sys_vault_register();                 /* SYS_VAULT_PUT / SYS_VAULT_GET (DDR-834) */
    sys_agentmem_register();              /* SYS_MEMORY_WRITE / SYS_MEMORY_READ (DDR-836) */
    sys_checkpoint_register();            /* SYS_CHECKPOINT_AGENT / SYS_RESUME_AGENT (DDR-837) */
    sys_rewrite_register();               /* SYS_APPROVE_CODE_REWRITE (DDR-842) */
    sys_experiment_register();            /* SYS_RUN_EXPERIMENT / SYS_EXP_RESULT (DDR-1034) */
    sys_audit_register();                 /* SYS_READ_AUDIT (DDR-842) */
    sys_acc_register();                   /* SYS_ACC_SEAL / SYS_ACC_OPEN (DDR-813,
                                           * linkable since DDR-827 raised the
                                           * stage-2 window to 1 MiB) */
    sys_fb_register();                    /* SYS_FB_* framebuffer surface (DDR-702) */
    sys_input_register();                 /* SYS_INPUT_POLL / SYS_MOUSE_POLL (DDR-703/705) */
    sys_surface_register();               /* SYS_SURFACE_* per-client surfaces (DDR-706) */

    syscall_init_ap();                    /* the BSP's MSRs — same helper as the APs */
}

/* ADR-031 cap-4: the SYSCALL machinery MSRs are PER-CPU — an AP without
 * EFER.SCE takes #UD on a user `syscall` instruction. The dispatch table above
 * is global (once, BSP); the MSRs are armed on every CPU that runs ring 3. */
void syscall_init_ap(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);            /* EFER.SCE */
    /* STAR: [47:32]=0x08 (SYSCALL CS, SS=+8=0x10); [63:48]=0x10 (SYSRET base:
     * SS=+8=0x18 user data, CS=+16=0x20 user code, RPL forced to 3). */
    wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);                         /* clear IF on kernel entry */
}
