/* kernel/idt.c
 * NEXUS — IDT setup and the CPU-exception dispatcher / panic handler (Phase 2a).
 *
 * idt_init() fills vectors 0..31 with the assembly stubs (arch/x86_64/isr.asm)
 * and loads the IDT. isr_dispatch() is the C side: #BP (vector 3) is treated as
 * a recoverable self-test (prints and returns); every other exception is a
 * kernel panic — it prints the component, the fault, a full register dump, CR2
 * for page faults, and a frame-pointer backtrace, then halts cleanly.
 */
#include "console.h"
#include "io.h"
#include "irq.h"
#include "sched.h"
#include "vdso_page.h"
#include "vmm_cow.h"
#include "vmm.h"          /* ADR-038: vmm_stack_fault + USER_STACK_* range */
#include "uaccess.h"    /* copyin — dump the bytes actually executing at a fault */
#include "regs.h"
#include "signal.h"
#include "ps2kbd.h"
#include "lapic.h"
#include <stdint.h>

struct idt_entry {
    uint16_t off_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern void *isr_stub_table[];          /* 32 stub addresses, from isr.asm */
extern void idt_load(struct idtr *ptr); /* from cpu.asm */

static struct idt_entry idt[256];

static const char *const exc_name[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint",
    "#OF overflow", "#BR bound range", "#UD invalid opcode", "#NM device n/a",
    "#DF double fault", "coprocessor overrun", "#TS invalid TSS", "#NP segment n/p",
    "#SS stack fault", "#GP general protection", "#PF page fault", "reserved",
    "#MF x87 fp", "#AC alignment", "#MC machine check", "#XM SIMD fp",
    "#VE virtualization", "#CP control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "#HV hypervisor", "#VC vmm comm", "#SX security", "reserved"
};

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void set_gate(int v, void *handler) {
    uint64_t a = (uint64_t)handler;
    idt[v].off_lo    = (uint16_t)(a & 0xFFFF);
    idt[v].selector  = 0x08;            /* kernel code selector (cpu.asm GDT) */
    idt[v].ist       = 0;
    idt[v].type_attr = 0x8E;            /* present, DPL0, 64-bit interrupt gate */
    idt[v].off_mid   = (uint16_t)((a >> 16) & 0xFFFF);
    idt[v].off_hi    = (uint32_t)((a >> 32) & 0xFFFFFFFF);
    idt[v].zero      = 0;
}

static struct idtr g_idtr;

void idt_init(void) {
    for (int i = 0; i < 64; i++)   /* 0..31 exc, 32..47 IRQs, 48 APIC timer,
                                    * 49 wake IPI, 50..63 MSI-X (DDR-714C1/C2;
                                    * DDR-771 extended the window to 63) */
        set_gate(i, isr_stub_table[i]);

    g_idtr.limit = (uint16_t)(sizeof(idt) - 1);
    g_idtr.base  = (uint64_t)&idt;
    idt_load(&g_idtr);
}

/* Load the (already-built) kernel IDT on an AP (DDR-SMP-3c-alpha): the
 * trampoline reaches long mode with the real-mode IDTR leftover, so the first
 * interrupt on an AP would triple-fault without this. */
void idt_load_ap(void) {
    idt_load(&g_idtr);
}

static void dump_line(const char *label, uint64_t v) {
    kputs(label);
    kputhex(v);
    kputs("\r\n");
}

/* Registerable handlers for IRQ 2..15 (device drivers, e.g. virtio). PCI INTx is
 * level-triggered and frequently SHARED (several virtio devices on one line), so
 * each line holds a small chain of handlers — each driver's handler reads its own
 * device's ISR (read-to-clear) and acts only on its own interrupt. Registration
 * is idempotent so a driver registered on the same line N times occupies 1 slot.
 * irq_handler_fn + irq_register are declared in irq.h. */
#define IRQ_MAX_HANDLERS 4
static irq_handler_fn irq_handlers[16][IRQ_MAX_HANDLERS];

/* MSI-X vectors 50..63 (DDR-714C1/C2, DDR-771): one handler per vector —
 * message-signaled interrupts are never shared, so no chain. EOI is LAPIC-only
 * (edge). 54 virtio-net, 55 virtio-input, 56..63 virtio-blk 0..7 (DDR-771).
 * 50..53 unused since the block window was relocated to 56..63. */
#define MSIX_VEC_BASE  50
#define MSIX_VEC_COUNT 14
static irq_handler_fn msix_handlers[MSIX_VEC_COUNT];

void msix_register(unsigned vector, irq_handler_fn fn) {
    if (vector >= MSIX_VEC_BASE && vector < MSIX_VEC_BASE + MSIX_VEC_COUNT)
        msix_handlers[vector - MSIX_VEC_BASE] = fn;
}

void net_poll_tick(void);   /* NET-B: lwip-port/pradyos_net.h (driven from the PIT) */
void virtio_blk_watchdog(void);  /* DDR-776: name a stuck blk request (B#3 diag) */

void irq_register(unsigned irq, irq_handler_fn fn) {
    if (irq >= 16)
        return;
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (irq_handlers[irq][i] == fn)     /* already registered on this line */
            return;
        if (!irq_handlers[irq][i]) {
            irq_handlers[irq][i] = fn;
            return;
        }
    }
}

/* The 100 Hz tick body, shared by the PIT (IRQ0) and APIC-timer (vector 48)
 * paths (DDR-714): global ticks, vDSO wall clock, preemption, lwIP timers, and
 * ring-3 signal delivery. The caller EOIs FIRST — sched_tick may switch away. */
/* DDR-889: g_ticks is `volatile uint64_t` (irq.c:5). `volatile` orders the
 * access but does NOT make a read-modify-write atomic, so two CPUs entering
 * timer_tick concurrently can both read N, both write N+1, and both match
 * (g_ticks % 500) == 0 — printing one heartbeat twice and LOSING a tick.
 * Measured in CI run 31843212987: of 12 [hb] lines, t=7500 and t=11000 each
 * appeared twice. A lost increment makes every g_ticks deadline run long, drifts
 * the vDSO wall clock slow, and can skip the %100 (blk watchdog) and %10 (lwIP)
 * arms outright. RELAXED is sufficient: this is a counter, and nothing infers
 * the ordering of other memory from its value. */
_Static_assert(sizeof(g_ticks) == 8, "DDR-889: g_ticks must stay 8 bytes (lock-free atomic add)");

static void timer_tick(struct regs *r) {
    /* DDR-952: use THIS tick's value, never a re-read of the global.
     * DDR-889 made the increment atomic, which stopped ticks being LOST, but
     * left every consumer below re-reading g_ticks. That is still a race, and
     * it has a worse failure mode than the duplicate heartbeat that motivated
     * it: two CPUs that increment to N and N+1 can BOTH re-read N (heartbeat
     * prints twice) or BOTH re-read N+1 (the value N is matched by nobody, so
     * the %100 blk watchdog and %10 lwIP arms are SKIPPED outright for that
     * tick). A skipped watchdog is silent; a doubled print is merely noisy.
     * __atomic_add_fetch already returns the post-increment value, so taking it
     * makes each tick value handled exactly once by exactly one CPU. */
    const uint64_t now = __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
    if (vdso_data) {                   /* IMP-C: advance the user-visible clock */
        vdso_data->seq++;              /* odd = write in progress */
        vdso_data->wall_time_ns += 10000000ull;   /* 10 ms per tick @100 Hz */
        vdso_data->seq++;              /* even = write complete  */
    }
    sched_tick();
    if ((now % 10u) == 0)         /* NET-B: drive lwIP timers ~every 100 ms */
        net_poll_tick();
    if ((now % 100u) == 0)        /* DDR-776: ~1 s stuck-blk-request scan */
        virtio_blk_watchdog();
    if ((now % 500u) == 0) {      /* DDR-777: ~5 s heartbeat — proves whether
                                       * g_ticks is still advancing during a B#3
                                       * stall (a g_ticks-bounded wait is only as
                                       * bounded as the timer). Evidence only:
                                       * no gate asserts on it. */
        uint64_t hbfl = console_line_lock();      /* DDR-963 §5 */
        kputs("[hb] t="); kputdec(now);
        { extern uint64_t g_thre_drops, g_rx_drops;
          kputs(" thre_drops="); kputdec(g_thre_drops);
          kputs(" rx_drops=");   kputdec(g_rx_drops); }
        /* DDR-890: how much did switch_wait_offcpu_sched spin in this window?
         * spins is the total across CPUs; max/cpu name the busiest one. This is
         * the data that decides whether the DDR-887 preemption-suppression
         * window is frequent enough to explain the post-fix CI pattern. */
        { extern uint32_t sched_take_spin_stats(uint32_t *, int *);
          uint32_t smax = 0; int scpu = 0;
          uint32_t stot = sched_take_spin_stats(&smax, &scpu);
          kputs(" spins="); kputdec(stot);
          kputs(" max=");   kputdec(smax);
          kputs(" cpu=");   kputdec((uint64_t)scpu); }
        /* DDR-893: calls/bails separate "one long wait" from "constant short
         * waits". A high call count is thrashing — the pick keeps landing on a
         * thread that is still on-CPU elsewhere. */
        { extern void sched_take_wait_stats(uint32_t *, uint32_t *);
          uint32_t wc = 0, wb = 0;
          sched_take_wait_stats(&wc, &wb);
          kputs(" calls="); kputdec(wc);
          kputs(" bails="); kputdec(wb); }
        /* DDR-936: the two silent gates between sched_unblock and the runqueue.
         * ubcas>0 => the thread was not BLOCKED when unblocked (ubst names the
         * state seen); ubrq>0 => rq_on leaked set so rq_push returned early.
         * Both zero on a run that still loses a thread => it WAS enqueued and
         * the defect is in the pick, not the enqueue. */
        { extern void sched_take_unblock_stats(uint32_t *, uint32_t *, uint32_t *);
          uint32_t uc = 0, ur = 0, us = 0;
          sched_take_unblock_stats(&uc, &ur, &us);
          kputs(" ubcas="); kputdec(uc);
          kputs(" ubrq=");  kputdec(ur);
          kputs(" ubst=");  kputdec(us); }
        /* DDR-941: rqmiss counts threads dropped by rq_take for being non-READY
         * at pick time — the strand path that increments neither ubcas nor ubrq.
         * btnedge counts press edges the input DRIVER saw; compare it against
         * the compositor's PRADYOS_BTN_STATE transitions to tell a lost event
         * from one coalesced between two ring-3 polls. Both cumulative. */
        { extern void sched_take_rqmiss_stats(uint32_t *, uint32_t *);
          extern uint32_t virtio_input_btn_edges(void);
          uint32_t rm = 0, rs = 0;
          sched_take_rqmiss_stats(&rm, &rs);
          kputs(" rqmiss="); kputdec(rm);
          kputs(" rqmst=");  kputdec(rs);
          kputs(" btnedge="); kputdec(virtio_input_btn_edges()); }
        /* DDR-942: rqdepth = entries still sitting in ready queues, rqcpus =
         * how many CPUs hold at least one. Depth staying >0 while a probe
         * reports "never ran" means the queue is not being drained. */
        { extern void sched_rq_depth(uint32_t *, uint32_t *);
          uint32_t rd = 0, rc2 = 0;
          sched_rq_depth(&rd, &rc2);
          kputs(" rqdepth="); kputdec(rd);
          kputs(" rqcpus=");  kputdec(rc2); }
        /* DDR-944: rqq = CPUs holding queue entries, rqpres = CPUs marked
         * present. Any bit set in rqq but clear in rqpres is a queue that
         * neither its own CPU nor any stealer will ever drain.
         * DDR-943: pmmfree/pmmtot — the eager 8 MiB user stack costs 2048
         * frames per process (elf.c:189-200) against 32768 on a default 128 MiB
         * QEMU machine, so ~15 processes exhaust RAM. Watch this collapse. */
        { extern void sched_rq_cpumasks(uint32_t *, uint32_t *);
          extern uint64_t pmm_free_page_count(void);
          extern uint64_t pmm_total_page_count(void);
          uint32_t rqq = 0, rqp = 0;
          sched_rq_cpumasks(&rqq, &rqp);
          kputs(" rqq=");     kputdec(rqq);
          kputs(" rqpres=");  kputdec(rqp);
          kputs(" pmmfree="); kputdec(pmm_free_page_count());
          kputs(" pmmtot=");  kputdec(pmm_total_page_count()); }
        /* DDR-947: on the A2 failure rqdepth pins (42, 11 heartbeats) on a
         * present ticking CPU with ubcas/ubrq/rqmiss all zero. preempt= climbing
         * while supp= stays flat means schedule() IS being called and not
         * switching; supp= climbing means g_in_switch is suppressing it; both
         * flat means the tick never reaches the preempt point. cur= names the
         * thread that is holding the CPU. */
        { extern void sched_take_preempt_stats(uint32_t *, uint32_t *);
          extern void sched_rq_depth(uint32_t *, uint32_t *);
          extern uint32_t sched_current_pid(void);
          extern const char *sched_current_name(void);
          uint32_t pt = 0, ps = 0, rd = 0;
          sched_take_preempt_stats(&pt, &ps);
          sched_rq_depth(&rd, 0);
          kputs(" preempt="); kputdec(pt);
          kputs(" supp=");    kputdec(ps);
          kputs(" curpid=");  kputdec((uint64_t)sched_current_pid());
          /* DDR-947: the NAME is a variable-length kputs inside the timer ISR.
           * Printing it every heartbeat coincided with the failure rate going
           * 2/12 -> 9/14, i.e. the instrument was heavy enough to move the
           * timing it was measuring (the hazard DDR-941 flagged for BTN_STATE
           * and this did not heed). Print it only when the queue is ABOVE
           * baseline — exactly the case it was added to diagnose — so the
           * steady state pays only the fixed-width numeric fields. */
          if (rd > 8u) { kputs(" cur="); kputs(sched_current_name()); } }
        kputs("\r\n");
        console_line_unlock(hbfl);                /* DDR-963 §5 */
    }
    if ((r->cs & 3) == 3)             /* PROC-C: deliver a pending signal */
        signal_deliver(r);            /* to the ring-3 thread we're returning to */
}

void isr_dispatch(struct regs *r) {
    /* AP wake IPI (DDR-SMP-3c-alpha): its only job is to break hlt; EOI + out. */
    if (r->vector == LAPIC_TIMER_VECTOR + 1) {
        lapic_eoi();
        return;
    }
    /* APIC timer (DDR-714): once armed it owns the tick (PIT IRQ0 masked).
     * cap-3: every CPU has its own LAPIC timer, but the GLOBAL tick side-effects
     * (g_ticks, the vDSO wall clock, lwIP timers) must advance once per real
     * tick — only the BSP runs timer_tick(); an AP's tick drives ONLY its own
     * preemption via sched_tick. (Ring-3 signal delivery is BSP-only until user
     * threads run on APs, cap-4.) */
    if (r->vector == LAPIC_TIMER_VECTOR) {
        lapic_eoi();                           /* EOI first: sched_tick may switch away */
        struct percpu *pc = this_cpu();
        if (pc && !pc->is_bsp) {
            sched_tick();                      /* AP: preemption only (no global tick) */
            if ((r->cs & 3) == 3)              /* cap-4: ring-3 runs on APs now — */
                signal_deliver(r);             /* deliver pending signals here too */
        } else {
            timer_tick(r);                     /* BSP (or pre-percpu): global tick */
        }
        return;
    }
    /* MSI-X device vectors (DDR-714C1): unshared, edge, LAPIC EOI only. */
    if (r->vector >= MSIX_VEC_BASE && r->vector < MSIX_VEC_BASE + MSIX_VEC_COUNT) {
        lapic_eoi();
        irq_handler_fn fn = msix_handlers[r->vector - MSIX_VEC_BASE];
        if (fn)
            fn();
        return;
    }
    /* Hardware IRQs (PIC remapped to 0x20..0x2F = vectors 32..47). */
    if (r->vector >= 32 && r->vector <= 47) {
        unsigned irqno = (unsigned)r->vector - 32;
        if (irqno == 0) {                      /* IRQ0: PIT timer — drives preemption */
            pic_eoi(r->vector);                /* EOI first: sched_tick may switch away */
            timer_tick(r);
            return;
        }
        if (irqno == 1) {                      /* IRQ1: PS/2 keyboard (DDR-703) */
            ps2kbd_isr();                      /* read 0x60, push ASCII to the ring */
        } else {
            for (int i = 0; i < IRQ_MAX_HANDLERS; i++)
                if (irq_handlers[irqno][i])    /* run every sharer on this line */
                    irq_handlers[irqno][i]();
        }
        pic_eoi(r->vector);                     /* EOI after the handler */
        return;
    }

    const char *name = (r->vector < 32) ? exc_name[r->vector] : "unknown";

    /* Fault originating in ring 3 (CPL in the saved CS == 3): a user program
     * misbehaved — a W^X violation (write to RX text), a guard-page hit, a bad
     * pointer, an illegal instruction, etc. Kill the process cleanly and keep the
     * kernel running, rather than panicking. This is the enforcement half of the
     * W^X / guard-page policy (ADR-021); until signals arrive in 5b this is
     * sys_exit(-1) semantics. The fault was in user code, so no kernel lock is
     * held and sched_exit can safely switch away (the dead thread's kernel-stack
     * ISR frame is abandoned, reaped later). */
    /* IMP-D: a ring-3 write to a present read-only page (error 0x7) on a COW page
     * is resolved by copy-on-write — resume the instruction. Non-COW pages return
     * -1 and fall through to the kill path, preserving W^X enforcement. */
    if (r->vector == 14 && (r->cs & 3) == 3 && (r->err_code & 0x7) == 0x7 &&
        current_thread && vmm_cow_fault(current_thread->cr3, read_cr2()) == 0)
        return;

    /* ADR-038: a ring-3 NOT-PRESENT fault (err bit0 == 0) inside the user stack
     * range grows the stack by one zeroed page. Note the COW arm above requires
     * bit0 == 1 (present), so the two cannot both claim a fault. Out-of-range
     * addresses — including the guard page, which sits BELOW USER_STACK_BOT —
     * and OOM both return -1 and fall through to the kill path below, so the
     * guard stays a guard and an allocation failure kills one thread rather
     * than panicking (S2). */
    if (r->vector == 14 && (r->cs & 3) == 3 && !(r->err_code & 0x1) &&
        current_thread && vmm_stack_fault(current_thread->cr3, read_cr2()) == 0)
        return;

    if (r->vector < 32 && (r->cs & 3) == 3) {
        /* DDR-963 §5: try, never block. This runs in EXCEPTION context, and a
         * fault taken INSIDE a line-locked region on this same CPU would
         * deadlock a blocking acquire — turning a diagnosable trap into a hang,
         * which is strictly worse than the splice being fixed. On a failed
         * acquire we print anyway and accept the spliced line. */
        uint64_t trfl = 0;
        int trheld = console_line_trylock(&trfl);
        kputs("[trap] user ");
        kputs(name);
        kputs(" pid=");
        kputdec(current_thread ? current_thread->pid : 0);
        /* Every probe ELF is linked at the same base, so a faulting rip alone does
         * NOT identify which binary faulted — four probes share entry 0x...b0.
         * Print the thread name elf_load already assigned, or three sessions get
         * spent disassembling the wrong ELF. */
        kputs(" name=");
        kputs(current_thread ? current_thread->name : "?");
        kputs(" rip=");
        kputhex(r->rip);
        kputs(" err=");                  /* #GP's selector is as diagnostic as #PF's flags */
        kputhex(r->err_code);
        if (r->vector == 14) {
            kputs(" cr2=");
            kputhex(read_cr2());
        }
        /* The bytes actually executing. A fault on an instruction that provably
         * cannot fault (e.g. #GP on `and $-16,%rsp`) means the loaded text is not
         * the linked text — this line is what distinguishes the two, and nothing
         * else in the log can. Read through the faulting thread's own mapping. */
        {
            unsigned char ib[16];
            if (copyin(ib, (const void __user *)(uintptr_t)r->rip, sizeof ib) ==
                (ssize_t)sizeof ib) {
                static const char hexd[] = "0123456789ABCDEF";
                kputs(" bytes@rip=");
                for (unsigned i = 0; i < sizeof ib; i++) {
                    kputc(hexd[(ib[i] >> 4) & 0xF]);
                    kputc(hexd[ib[i] & 0xF]);
                    kputc(' ');
                }
            } else {
                kputs(" bytes@rip=<unreadable>");
            }
            /* Also dump the start of the containing page. If the text has been
             * overwritten by a data pattern, where index 0 of that pattern lands
             * tells us whether the writer was page-aligned (a reused frame) or
             * offset (a stray copy), and those have different root causes. */
            unsigned char pb[16];
            uint64_t base = r->rip & ~0xFFFull;
            if (copyin(pb, (const void __user *)(uintptr_t)base, sizeof pb) ==
                (ssize_t)sizeof pb) {
                static const char hexd2[] = "0123456789ABCDEF";
                kputs(" page+0=");
                for (unsigned i = 0; i < sizeof pb; i++) {
                    kputc(hexd2[(pb[i] >> 4) & 0xF]);
                    kputc(hexd2[pb[i] & 0xF]);
                    kputc(' ');
                }
            }
        }
        kputs(" — killing process\r\n");
        /* Release BEFORE sched_exit: it never returns, so an unlock placed
         * after it would strand the lock held forever and hang every later
         * printer on every CPU. */
        if (trheld) console_line_unlock(trfl);
        sched_exit(-1);                  /* zombie (status -1) + switches away; never returns */
    }

    if (r->vector == 3) {                /* recoverable self-test */
        kputs("[#BP] breakpoint at RIP=");
        kputhex(r->rip);
        kputs(" — IDT works, resuming\r\n");
        return;
    }

    kputs("\r\n*** NEXUS KERNEL PANIC ***\r\n");
    kputs("component: NEXUS isr\r\n");
    kputs("exception: ");
    kputs(name);
    kputs("  vector=");
    kputhex(r->vector);
    kputs("  error=");
    kputhex(r->err_code);
    kputs("\r\n");

    dump_line("RIP=", r->rip);
    dump_line("CS =", r->cs);
    dump_line("RFLAGS=", r->rflags);
    dump_line("RSP=", r->rsp);
    if (r->vector == 14)
        dump_line("CR2=", read_cr2());

    dump_line("RAX=", r->rax);
    dump_line("RBX=", r->rbx);
    dump_line("RCX=", r->rcx);
    dump_line("RDX=", r->rdx);
    dump_line("RSI=", r->rsi);
    dump_line("RDI=", r->rdi);
    dump_line("RBP=", r->rbp);
    dump_line("R8 =", r->r8);
    dump_line("R9 =", r->r9);
    dump_line("R10=", r->r10);
    dump_line("R11=", r->r11);
    dump_line("R12=", r->r12);
    dump_line("R13=", r->r13);
    dump_line("R14=", r->r14);
    dump_line("R15=", r->r15);

    kputs("backtrace:\r\n");
    uint64_t *bp = (uint64_t *)r->rbp;
    for (int i = 0; i < 8 && bp; i++) {
        kputs("  ");
        kputhex(bp[1]);              /* saved return address */
        kputs("\r\n");
        bp = (uint64_t *)bp[0];      /* previous frame */
    }

    kputs("halting.\r\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}
