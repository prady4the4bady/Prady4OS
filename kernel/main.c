/* kernel/main.c
 * ===========================================================================
 * NEXUS kernel — long-mode entry point (Phase 2a).
 *
 * Reached in 64-bit long mode, ring 0, on a flat identity map of the low 1 GiB
 * set up by the bootloader. This slice installs the kernel's own GDT and an IDT
 * with handlers for all 32 CPU exceptions, then runs a recoverable #BP self-test
 * to prove the IDT works. Still no allocator, no scheduler, no hardware
 * interrupts (no PIC/APIC) — those are later slices.
 *
 * Built -ffreestanding -mgeneral-regs-only (no SSE), -fno-omit-frame-pointer.
 * ===========================================================================
 */
#include "console.h"
#include "boot_info.h"
#include "irq.h"
#include "pmm.h"
#include "kheap.h"
#include "vmm.h"
#include "cap.h"
#include "sched.h"
#include "ipc.h"

extern void gdt_init(void);    /* arch/x86_64/cpu.asm */
extern void idt_init(void);    /* kernel/idt.c        */

/* A subsystem guards an operation by demanding a capability with the right. */
static int demo_file_read(struct cap_table *t, cap_t h) {
    if (!cap_validate(t, h, CAP_FILE_R))
        return -1;                       /* -EPERM: no read capability */
    return 0;                            /* would read here */
}

static int cap_check(const char *label, uint32_t got, uint32_t expect) {
    kputs("  ");
    kputs(label);
    kputs(got == expect ? " ok\r\n" : " FAIL\r\n");
    return got == expect;
}

static void cap_test(void) {
    kputs("NEXUS: capability system (NCS) tests\r\n");
    struct cap_table *t = cap_table_create();
    struct cap_table *t2 = cap_table_create();
    int pass = 0, total = 0;

    cap_t h = cap_create(t, RES_FILE, 0x1234, CAP_FILE_R | CAP_FILE_W);
    total++; pass += cap_check("validate R", cap_validate(t, h, CAP_FILE_R), 1);
    total++; pass += cap_check("reject NET (not granted)", cap_validate(t, h, CAP_NET), 0);
    total++; pass += cap_check("guarded read allowed", demo_file_read(t, h) == 0, 1);

    cap_t hr = cap_restrict(t, h, CAP_FILE_R);
    total++; pass += cap_check("restricted: W denied", cap_validate(t, hr, CAP_FILE_W), 0);
    total++; pass += cap_check("restricted: R kept", cap_validate(t, hr, CAP_FILE_R), 1);

    /* Delegating a read-only cap while asking for R|W must NOT amplify. */
    cap_t hd = cap_delegate(t, hr, t2, CAP_FILE_R | CAP_FILE_W);
    total++; pass += cap_check("delegate cannot amplify W", cap_validate(t2, hd, CAP_FILE_W), 0);
    total++; pass += cap_check("delegate keeps R", cap_validate(t2, hd, CAP_FILE_R), 1);

    cap_revoke(t, h);
    total++; pass += cap_check("revoked handle invalid", cap_validate(t, h, CAP_FILE_R), 0);
    total++; pass += cap_check("guarded read now denied", demo_file_read(t, h) == -1, 1);
    total++; pass += cap_check("delegated cap survives revoke", cap_validate(t2, hd, CAP_FILE_R), 1);
    total++; pass += cap_check("restricted cap survives revoke", cap_validate(t, hr, CAP_FILE_R), 1);

    kputs("NEXUS: NCS ");
    kputdec((uint64_t)pass);
    kputs("/");
    kputdec((uint64_t)total);
    kputs(pass == total ? " passed\r\n" : " FAILED\r\n");

    cap_table_destroy(t);
    cap_table_destroy(t2);
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate the TSC against the PIT (100 Hz). Runs before the scheduler is up,
 * so the calibration loop is not preempted. */
static uint64_t calibrate_tsc_hz(void) {
    uint64_t t = g_ticks;
    while (g_ticks == t) { }              /* align to a tick edge */
    uint64_t start = g_ticks;
    uint64_t c0 = rdtsc();
    while (g_ticks < start + 20) { }      /* 20 ticks = 200 ms */
    uint64_t c1 = rdtsc();
    return (c1 - c0) * 5;                  /* per 0.2 s -> per second */
}

/* Bench partner: immediately hands the CPU back, so an idle yield() round-trips
 * through exactly two context switches. */
static void bench_partner(void *arg) {
    (void)arg;
    for (;;)
        yield();
}

static void bench_ctx_switch(uint64_t tsc_hz) {
    const uint64_t rounds = 100000;
    uint64_t c0 = rdtsc();
    for (uint64_t i = 0; i < rounds; i++)
        yield();                          /* idle -> partner -> idle = 2 switches */
    uint64_t c1 = rdtsc();
    uint64_t switches = rounds * 2;
    uint64_t cyc = (c1 - c0) / switches;
    kputs("NEXUS: context_switch ~");
    kputdec(cyc);
    kputs(" cycles (~");
    kputdec(tsc_hz ? (cyc * 1000000000ull / tsc_hz) : 0);
    kputs(" ns)  [target <= 1500 ns]\r\n");
}

/* --- IPC demo: a receiver blocks on an endpoint, a sender delivers ---------
 * Both operations are capability-gated; each thread holds only the right it
 * needs. Demonstrates block/wakeup AND capability enforcement. */
static struct ipc_endpoint demo_ep;
#define DEMO_EP_ID 0xABCDull

static void ipc_receiver_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    uint64_t buf[IPC_MSG_WORDS];

    kputs("[recv] blocking on endpoint (no message yet)\r\n");
    if (ipc_recv(current_thread->caps, cap, &demo_ep, buf) == 0) {
        kputs("[recv] received: ");
        kputhex(buf[0]);
        kputs(" ");
        kputhex(buf[1]);
        kputs("\r\n");
    } else {
        kputs("[recv] DENIED\r\n");
    }

    /* Enforcement: a recv-only cap must not be usable to send. */
    uint64_t m[IPC_MSG_WORDS] = { 0, 0, 0, 0 };
    int r = ipc_send(current_thread->caps, cap, &demo_ep, m);
    kputs(r != 0 ? "[recv] send with recv-only cap correctly DENIED\r\n"
                 : "[recv] ERROR: recv-only cap was allowed to send!\r\n");
}

static void ipc_sender_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    for (volatile uint64_t d = 0; d < 60000000ull; d++) { }  /* let recv block first */

    uint64_t msg[IPC_MSG_WORDS] = { 0xFEEDFACECAFEBEEFull, 0x0102030405060708ull, 0, 0 };
    kputs("[send] delivering message\r\n");
    int r = ipc_send(current_thread->caps, cap, &demo_ep, msg);
    kputs(r == 0 ? "[send] delivered\r\n" : "[send] DENIED\r\n");
}

static void ipc_demo(void) {
    ipc_endpoint_init(&demo_ep, DEMO_EP_ID);
    struct tcb *recv = sched_create(ipc_receiver_thread, 0, "recv");
    struct tcb *send = sched_create(ipc_sender_thread, 0, "send");
    if (!recv || !send) {
        kputs("NEXUS: ipc_demo — thread create failed\r\n");
        return;
    }
    /* Mint each thread a single, resource-bound capability with only its right. */
    recv->arg = (void *)(uintptr_t)cap_create(recv->caps, RES_IPC, DEMO_EP_ID, CAP_IPC_RECV);
    send->arg = (void *)(uintptr_t)cap_create(send->caps, RES_IPC, DEMO_EP_ID, CAP_IPC_SEND);
    kputs("NEXUS: IPC demo — capability-gated recv/send threads\r\n");
}

static void sched_demo(void) {
    uint64_t tsc_hz = calibrate_tsc_hz();
    kputs("NEXUS: TSC ~");
    kputdec(tsc_hz / 1000000);
    kputs(" MHz\r\n");

    sched_init();
    sched_create(bench_partner, 0, "bench");
    bench_ctx_switch(tsc_hz);

    ipc_demo();
    kputs("NEXUS: scheduler + IPC live\r\n");

    for (;;)                               /* this context is now the idle thread */
        __asm__ volatile("hlt");
}

static void vmm_test(void) {
    const uint64_t va = 0xFFFF800000000000ull;   /* unused PML4 slot (256) */
    uint64_t pg = pmm_alloc_page();
    if (!pg) {
        kputs("NEXUS: vmm test — no frame\r\n");
        return;
    }
    uint64_t before = kheap_outstanding();
    if (vmm_map(va, pg, VMM_RW) != 0) {
        kputs("NEXUS: vmm_map FAILED\r\n");
        pmm_free_page(pg);
        return;
    }
    volatile uint64_t *p = (volatile uint64_t *)va;
    p[0] = 0xCAFEBABEDEADBEEFull;
    uint64_t rb = p[0];

    kputs("NEXUS: vmm_map va=");
    kputhex(va);
    kputs(" pa=");
    kputhex(pg);
    kputs(" readback=");
    kputhex(rb);
    kputs(rb == 0xCAFEBABEDEADBEEFull ? "  (OK)\r\n" : "  (FAIL)\r\n");

    vmm_unmap(va);
    pmm_free_page(pg);
    uint64_t after = kheap_outstanding();
    kputs("NEXUS: vmm unmap reclaim — outstanding ");
    kputhex(before);
    kputs(" -> ");
    kputhex(after);
    kputs(after == before ? "  (clean)\r\n" : "  (LEAK!)\r\n");
}

static void kheap_stress(void) {
    kheap_init();
    uint64_t base = kheap_outstanding();

    /* Mixed-size churn: some land in slab caches, some are whole-page large. */
    void *p[64];
    for (int i = 0; i < 64; i++) {
        size_t sz = (size_t)(((unsigned)i * 37u + 8u) & 0xFFFu) + 1u;  /* 1..4096 */
        p[i] = kmalloc(sz);
        if (p[i]) {
            ((volatile unsigned char *)p[i])[0] = (unsigned char)i;
            ((volatile unsigned char *)p[i])[sz - 1] = (unsigned char)~i;
        }
    }
    for (int i = 0; i < 64; i++)
        kfree(p[i]);

    /* Dedicated object pools. */
    void *a = pcb_alloc(), *b = cap_alloc(), *c = ipc_alloc(), *d = ptnode_alloc();
    pcb_free(a); cap_free(b); ipc_free(c); ptnode_free(d);

    uint64_t after = kheap_outstanding();
    kputs("NEXUS: kheap stress — outstanding base=");
    kputhex(base);
    kputs(" after=");
    kputhex(after);
    kputs(after == base ? "  (no leak)\r\n" : "  (LEAK!)\r\n");
}

static void pmm_selftest(const struct boot_info *bi) {
    pmm_init(bi);
    kputs("NEXUS: PMM (buddy) free frames=");
    kputhex(pmm_free_page_count());
    kputs("\r\n");

    uint64_t start = pmm_free_page_count();
    uint64_t a = pmm_alloc_page();        /* order 0 */
    uint64_t b = pmm_alloc_pages(3);      /* order 3 = 8 frames */
    kputs("  alloc 1 frame -> ");
    kputhex(a);
    kputs("\r\n  alloc 8 frames -> ");
    kputhex(b);
    kputs("\r\n  free frames after alloc=");
    kputhex(pmm_free_page_count());
    kputs("\r\n");

    pmm_free_pages(b, 3);
    pmm_free_page(a);
    uint64_t end = pmm_free_page_count();
    kputs("  free frames after release=");
    kputhex(end);
    kputs(end == start ? "  (balanced)\r\n" : "  (LEAK!)\r\n");
}

static void print_boot_info(const struct boot_info *bi) {
    if (bi->magic != BOOT_INFO_MAGIC) {
        kputs("NEXUS: WARNING bad boot_info magic=");
        kputhex(bi->magic);
        kputs("\r\n");
        return;
    }
    kputs("NEXUS: boot_info OK  vendor=");
    kputs(bi->cpu_vendor);
    kputs("  long_mode=");
    kputhex(bi->long_mode);
    kputs("\r\n");
    kputs("NEXUS: E820 map, entries=");
    kputhex(bi->e820_count);
    kputs("\r\n");
    for (uint32_t i = 0; i < bi->e820_count; i++) {
        kputs("  base=");
        kputhex(bi->e820[i].base);
        kputs(" len=");
        kputhex(bi->e820[i].len);
        kputs(" type=");
        kputhex(bi->e820[i].type);
        kputs("\r\n");
    }
}

void kmain(struct boot_info *bi) {
    kputs("NEXUS: entered kmain (64-bit long mode, ring 0)\r\n");

    print_boot_info(bi);

    gdt_init();
    kputs("NEXUS: kernel GDT loaded\r\n");

    idt_init();
    kputs("NEXUS: IDT loaded (48 vectors: 32 exceptions + 16 IRQ)\r\n");

    kvga_line("NEXUS KERNEL OK", 1);
    kputs("NEXUS KERNEL OK\r\n");

    /* Self-test: a breakpoint must be caught by the IDT and resume execution. */
    kputs("NEXUS: IDT self-test — executing int3...\r\n");
    __asm__ volatile("int3");
    kputs("NEXUS: resumed after int3 — exception handling verified\r\n");

    /* Hardware interrupts: PIC + PIT, then enable and watch the clock tick. */
    pic_remap();
    pit_init(100);                       /* 100 Hz */
    kputs("NEXUS: PIC remapped, PIT @100Hz; enabling interrupts (sti)\r\n");
    __asm__ volatile("sti");

    while (g_ticks < 5)                   /* prove IRQ0 actually fires */
        __asm__ volatile("hlt");
    kputs("NEXUS: timer IRQ alive, ticks=");
    kputhex(g_ticks);
    kputs("\r\n");

    pmm_selftest(bi);
    kheap_stress();
    vmm_test();
    cap_test();

    kputs("NEXUS: starting scheduler\r\n");
    sched_demo();                          /* never returns (becomes the idle thread) */
}
