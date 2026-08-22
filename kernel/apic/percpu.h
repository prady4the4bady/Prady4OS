/* kernel/apic/percpu.h — per-CPU identity area (ADR-030 stage 2, DDR-SMP-2).
 *
 * LAPIC-ID-indexed (NOT %gs-based: without SWAPGS discipline a ring-3 gs
 * selector reload could clobber a MSR-set base and break the kernel — see the
 * DDR). this_cpu() is safe from any context on any CPU. Fields grow with the
 * ADR-030 stage that uses them.
 */
#pragma once
#include <stdint.h>

#define PERCPU_MAX 16

struct tcb;

struct percpu {
    struct percpu *self;   /* @0: this_cpu() reads %gs:0 (DDR-SMP-3a)            */
    struct tcb *current;   /* @8: this CPU's running thread (DDR-SMP-3b)         */
    uint64_t kstack_top;   /* @16: SYSCALL stack switch — asm reads [gs:16]      */
    void (*job)(void);     /* @24: single-slot work mailbox (DDR-SMP-3c-alpha)   */
    uint32_t cpu_idx;      /* MADT roster index (0 = BSP)                        */
    uint32_t apic_id;      /* this CPU's LAPIC id                                */
    uint8_t  present;
    uint8_t  is_bsp;       /* 1 on the bootstrap processor (cap-2b D4); travels
                            * through the percpu_init_bsp migration copy         */
    uint64_t ticks;        /* cap-3: this CPU's LAPIC-timer ticks (preemption proof) */

    /* cap-4 (ADR-031 D5): per-CPU SYSCALL-entry register snapshot. Written by
     * syscall_entry.asm via %gs at the FIXED offsets below (static-asserted in
     * percpu.c — keep in lockstep); read by the fork paths on the same CPU.
     * Replaces the syscall_user_* globals two CPUs would clobber. */
    uint64_t u_rsp;        /* @56  user RSP at syscall entry                     */
    uint64_t u_rip;        /* @64  user return RIP (fork child resume point)     */
    uint64_t u_rbx;        /* @72  callee-saved snapshot for full-register fork  */
    uint64_t u_rbp;        /* @80                                                */
    uint64_t u_r12;        /* @88                                                */
    uint64_t u_r13;        /* @96                                                */
    uint64_t u_r14;        /* @104                                               */
    uint64_t u_r15;        /* @112                                               */
    uint64_t u_rflags;     /* @120 user RFLAGS (R11) at entry                    */

    /* rq-2: appended AFTER the asm-consumed u_* block (whose offsets are
     * static-asserted) — the thread THIS CPU last switched away from. Whoever
     * resumes here release-stores prev->on_cpu = -1 (finish_task_switch) once
     * context_switch has left that thread's stack: the off-CPU handshake. */
    struct tcb *prev;
    volatile uint8_t idle;  /* rq-3: 1 while this CPU is in the idle-loop hlt wait —
                             * a directed wake IPI (smp_resched_one) breaks it. */

    /* DDR-981: AP-freeze NMI probe. Appended AFTER every asm-consumed offset
     * (the u_* block is static-asserted in percpu.c), so this cannot move them.
     * Protocol: the BSP latches ONE stalled AP per boot, sets nmi_dump = 1 and
     * NMIs it; that AP's NMI handler fills the d_* fields and release-stores
     * nmi_dump = 2; the BSP prints from its own heartbeat and stores 3.
     * The AP never prints — it may be wedged holding g_line_lock, which is the
     * whole reason this is a stash-and-relay rather than a dump-in-place. */
    volatile uint8_t nmi_dump;  /* 0 idle | 1 armed | 2 filled | 3 printed */
    uint8_t  d_shot;            /* which sample this is (1..DUMP_SHOTS) */
    uint8_t  d_btn;             /* how many d_bt[] entries are valid */
    uint64_t d_rip, d_rsp, d_rbp, d_rflags;
    uint64_t d_bt[4];           /* return addresses, walked from the frame chain */
    uint32_t d_cs, d_lvt, d_tpr, d_svr, d_isr48, d_irr48, d_pid;
};

/* Another CPU's entry by roster index (the BSP writes an AP's mailbox). */
struct percpu *percpu_get(uint32_t cpu_idx);

/* Claim slot 0 for the BSP before the scheduler's first tick (kmain top);
 * percpu_init_bsp later fills the LAPIC id (migrating slots if the BSP's
 * roster index isn't 0). DDR-SMP-3b D3. */
void percpu_init_early(void);

/* Record the calling CPU's identity (BSP after lapic_init; APs in smp_ap_entry). */
void percpu_init_cpu(uint32_t cpu_idx);

/* BSP convenience: find its MADT roster index by LAPIC id and record it. */
void percpu_init_bsp(void);

/* The calling CPU's percpu entry, resolved by LAPIC id; NULL before init. */
struct percpu *this_cpu(void);
