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
    for (int i = 0; i < 50; i++)   /* 0..31 exc, 32..47 IRQs, 48 APIC timer, 49 wake IPI */
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

void net_poll_tick(void);   /* NET-B: lwip-port/pradyos_net.h (driven from the PIT) */

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
static void timer_tick(struct regs *r) {
    g_ticks++;
    if (vdso_data) {                   /* IMP-C: advance the user-visible clock */
        vdso_data->seq++;              /* odd = write in progress */
        vdso_data->wall_time_ns += 10000000ull;   /* 10 ms per tick @100 Hz */
        vdso_data->seq++;              /* even = write complete  */
    }
    sched_tick();
    if ((g_ticks % 10u) == 0)         /* NET-B: drive lwIP timers ~every 100 ms */
        net_poll_tick();
    if ((r->cs & 3) == 3)             /* PROC-C: deliver a pending signal */
        signal_deliver(r);            /* to the ring-3 thread we're returning to */
}

void isr_dispatch(struct regs *r) {
    /* AP wake IPI (DDR-SMP-3c-alpha): its only job is to break hlt; EOI + out. */
    if (r->vector == LAPIC_TIMER_VECTOR + 1) {
        lapic_eoi();
        return;
    }
    /* APIC timer (DDR-714): once armed it owns the tick (PIT IRQ0 masked). */
    if (r->vector == LAPIC_TIMER_VECTOR) {
        lapic_eoi();                           /* EOI first: sched_tick may switch away */
        timer_tick(r);
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

    if (r->vector < 32 && (r->cs & 3) == 3) {
        kputs("[trap] user ");
        kputs(name);
        kputs(" pid=");
        kputdec(current_thread ? current_thread->pid : 0);
        kputs(" rip=");
        kputhex(r->rip);
        if (r->vector == 14) {
            kputs(" cr2=");
            kputhex(read_cr2());
            kputs(" err=");
            kputhex(r->err_code);
        }
        kputs(" — killing process\r\n");
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
