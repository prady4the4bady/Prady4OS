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
#include "bcast.h"
#include "tss.h"
#include "syscall.h"
#include "string.h"
#include "acpi.h"
#include "lapic.h"
#include "smp.h"
#include "percpu.h"
#include "pcie.h"
#include "blk.h"
#include "virtio_blk.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include "virtio_net.h"
#include "vfs.h"
#include "fat32.h"
#include "sfs.h"
#include "ext4.h"
#include "rtc.h"
#include "elf.h"
#include "uaccess.h"
#include "errno.h"
#include "cpu_mitigations.h"
#include "vdso_page.h"
#include "vmm_cow.h"

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

/* Bench partner: hands the CPU back so an idle yield() round-trips through
 * exactly two context switches. Exits once the benchmark is done so it stops
 * consuming scheduler slots. */
static volatile int bench_done = 0;
static void bench_partner(void *arg) {
    (void)arg;
    while (!bench_done)
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
    for (int i = 0; i < 8; i++)              /* let the receiver block first */
        yield();

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

/* --- Async SPSC ring demo: producer and consumer decoupled by the ring ----- */
#define RING_EP_ID 0x5170ull
#define RING_N 200
static struct ipc_ring demo_ring;

static void ring_producer_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    for (uint64_t i = 1; i <= RING_N; ) {
        int r = ipc_ring_push(current_thread->caps, cap, &demo_ring, i);
        if (r == 0)
            i++;                 /* pushed; advance */
        else if (r == 1)
            yield();             /* ring full; let the consumer drain */
        else {
            kputs("[ring prod] cap DENIED\r\n");
            return;
        }
    }
    kputs("[ring prod] pushed 1..200\r\n");
}

static void ring_consumer_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    uint64_t expect = 1, got, count = 0, errors = 0;
    while (count < RING_N) {
        int r = ipc_ring_pop(current_thread->caps, cap, &demo_ring, &got);
        if (r == 0) {
            if (got != expect)
                errors++;
            expect++;
            count++;
        } else if (r == 1) {
            yield();             /* ring empty; let the producer fill */
        } else {
            kputs("[ring cons] cap DENIED\r\n");
            return;
        }
    }
    kputs("[ring cons] received 200 in-order, errors=");
    kputdec(errors);
    kputs(errors == 0 ? "  (OK)\r\n" : "  (FAIL)\r\n");
}

static void ring_demo(void) {
    ipc_ring_init(&demo_ring, RING_EP_ID);
    struct tcb *prod = sched_create(ring_producer_thread, 0, "prod");
    struct tcb *cons = sched_create(ring_consumer_thread, 0, "cons");
    if (!prod || !cons) {
        kputs("NEXUS: ring_demo — thread create failed\r\n");
        return;
    }
    prod->arg = (void *)(uintptr_t)cap_create(prod->caps, RES_IPC, RING_EP_ID, CAP_IPC_SEND);
    cons->arg = (void *)(uintptr_t)cap_create(cons->caps, RES_IPC, RING_EP_ID, CAP_IPC_RECV);
    kputs("NEXUS: async SPSC ring demo — producer + consumer\r\n");
}

/* --- Sovereign broadcast bus demo: 2 filtered subscribers + a publisher ----- */
#define BUS_EP_ID 0xB05ull
static struct bcast_bus demo_bus;
static struct bcast_subscriber sub_a, sub_b;
static volatile int subs_ready = 0;       /* subscribers bump this once subscribed */

static void sub_approvals_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    bcast_subscribe(current_thread->caps, cap, &demo_bus, &sub_a,
                    EVT_APPROVAL_REQUEST | EVT_MODE_CHANGE);
    subs_ready++;
    for (int i = 0; i < 2; i++) {
        struct bcast_event e;
        bcast_wait(&sub_a, &e);
        kputs("[sub-approve] event type=");
        kputhex(e.type);
        kputs(" payload=");
        kputhex(e.payload);
        kputs("\r\n");
    }
    kputs("[sub-approve] done (got APPROVAL+MODE, not ALERT)\r\n");
}

static void sub_alerts_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    bcast_subscribe(current_thread->caps, cap, &demo_bus, &sub_b, EVT_RESOURCE_ALERT);
    subs_ready++;
    struct bcast_event e;
    bcast_wait(&sub_b, &e);
    kputs("[sub-alert] event type=");
    kputhex(e.type);
    kputs("\r\n[sub-alert] done (got only ALERT)\r\n");
}

static void publisher_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    while (subs_ready < 2)                    /* wait until both subscribers register */
        yield();
    bcast_publish(current_thread->caps, cap, &demo_bus, EVT_APPROVAL_REQUEST, 0x1111);
    bcast_publish(current_thread->caps, cap, &demo_bus, EVT_RESOURCE_ALERT, 0x2222);
    bcast_publish(current_thread->caps, cap, &demo_bus, EVT_MODE_CHANGE, 0x3333);
    kputs("[pub] published APPROVAL, ALERT, MODE\r\n");
}

static void bus_demo(void) {
    bcast_bus_init(&demo_bus, BUS_EP_ID);
    struct tcb *a = sched_create(sub_approvals_thread, 0, "sub-approve");
    struct tcb *b = sched_create(sub_alerts_thread, 0, "sub-alert");
    struct tcb *p = sched_create(publisher_thread, 0, "pub");
    if (!a || !b || !p) {
        kputs("NEXUS: bus_demo — thread create failed\r\n");
        return;
    }
    a->arg = (void *)(uintptr_t)cap_create(a->caps, RES_IPC, BUS_EP_ID, CAP_IPC_RECV);
    b->arg = (void *)(uintptr_t)cap_create(b->caps, RES_IPC, BUS_EP_ID, CAP_IPC_RECV);
    p->arg = (void *)(uintptr_t)cap_create(p->caps, RES_IPC, BUS_EP_ID, CAP_BROADCAST);
    kputs("NEXUS: broadcast bus demo — 2 filtered subscribers + publisher\r\n");
}

/* --- Phase 5a ring-3 user programs -----------------------------------------
 * The static ELFs are embedded by arch/x86_64/user_image.asm. Each is written
 * to a freshly formatted SFS volume and then loaded BACK from SFS into its own
 * W^X address space (ADR-021) — the legacy RWX-mapped demo is gone. `hello`
 * prints from ring 3 and exits; `wxviol` is the W^X negative regression. */
extern const unsigned char hello_elf[];
extern const unsigned char hello_elf_end[];
extern const unsigned char wx_elf[];
extern const unsigned char wx_elf_end[];
extern const unsigned char systest_elf[];
extern const unsigned char systest_elf_end[];
extern const unsigned char exectest_elf[];
extern const unsigned char exectest_elf_end[];
extern const unsigned char tlstest_elf[];        /* PROC-D: SET_TLS + WRITEV probe */
extern const unsigned char tlstest_elf_end[];
extern const unsigned char cmusl_elf[];          /* PROC-D: first musl C program */
extern const unsigned char cmusl_elf_end[];
extern const unsigned char fputest_elf[];        /* 5d: FPU context-switch test */
extern const unsigned char fputest_elf_end[];
extern const unsigned char init_elf[];           /* 5d: pradyos-init (PID 1) */
extern const unsigned char init_elf_end[];
extern const unsigned char prism_elf[];          /* 5e: PRISM shell */
extern const unsigned char prism_elf_end[];
extern const unsigned char aether_daemon_elf[];  /* L6: AETHER daemon (PID 2) */
extern const unsigned char aether_daemon_elf_end[];
extern const unsigned char agent_base_elf[];     /* L6: AETHER agent template */
extern const unsigned char agent_base_elf_end[];
extern const unsigned char inputtest_elf[];      /* L7: ring-3 keyboard reader */
extern const unsigned char inputtest_elf_end[];
extern const unsigned char compositor_elf[];     /* L7: sovereign-desktop compositor */
extern const unsigned char compositor_elf_end[];
extern const unsigned char surfacetest_elf[];    /* L7: per-client surface test window */
extern const unsigned char surfacetest_elf_end[];
void aether_set_spawn_hook(long (*fn)(const char *task));  /* kernel/syscall/sys_aether.c */
void net_init(void);                             /* NET-B: lwip-port/pradyos_net.h */
void aether_init(void);                          /* Layer 6: kernel/aether/aether.c */
void aether_selftest(void);
void aether_sectest(void);

/* Write an embedded ELF to SFS, read it BACK from SFS, and load it as a ring-3
 * process. Genuinely exercises the filesystem load path (the bytes elf_load
 * parses come from sfs_read, not the embedded image). */
/* `sovereign` grants CAP_SOVEREIGN BEFORE the thread's first run: elf_load now
 * returns the thread BLOCKED, and the unblock happens only after the authority
 * flags are set (DDR-boot-authority-race). */
static struct tcb *user_boot_from_sfs(cap_t cap, int smnt, const char *fname,
                               const unsigned char *elf, const unsigned char *elf_end,
                               int sovereign) {
    uint64_t elen = (uint64_t)(elf_end - elf);
    struct vfs_file ef;
    if (vfs_create(cap, smnt, fname, &ef) != 0 ||
        vfs_write(cap, &ef, 0, elf, (uint32_t)elen) != (int)elen) {
        kputs("[user] SFS write failed for ");
        kputs(fname);
        kputs("\r\n");
        return 0;
    }
    struct vfs_file rf;
    /* 256 KiB read buffer (order-6) from the PMM pool, matching the EXEC_MAX
     * user-ELF budget (PROC-D / ADR-023). Big buffers come from the PMM pool,
     * never BSS (the low-mem image cap). Freed after the loader copies the
     * image into the new address space. */
    enum { USER_ELF_MAX = 256u * 1024u };
    uint64_t buf = pmm_alloc_pages(6);
    if (!buf)
        return 0;
    if (vfs_open(cap, smnt, fname, &rf) != 0) {
        pmm_free_pages(buf, 6);
        return 0;
    }
    uint32_t want = (elen > USER_ELF_MAX) ? USER_ELF_MAX : (uint32_t)elen;
    int n = vfs_read(cap, &rf, 0, (void *)(uintptr_t)buf, want);
    struct tcb *ut = 0;
    int lr = (n > 0)
        ? elf_load((void *)(uintptr_t)buf, (uint64_t)n, fname, &ut)
        : ELF_E_ARGS;
    pmm_free_pages(buf, 6);
    if (lr == ELF_OK) {
        if (sovereign)
            ut->is_sovereign = 1;      /* authority BEFORE the first run */
        sched_unblock(ut);             /* elf_load returns the thread BLOCKED */
        kputs("[user] ELF loaded from SFS; ring-3 thread spawned\r\n");
        return ut;
    }
    kputs("[user] ELF load FAILED rc=");
    kputdec((uint64_t)(-lr));
    kputs("\r\n");
    return 0;
}

/* DDR-SMP-3c-alpha: the boot-time AP-dispatch proof — runs ON each AP. */
static void smp_test_job(void) {
    kputs("[smp] cpu ");
    kputdec(this_cpu()->cpu_idx);
    kputs(" job OK\r\n");
}

/* DDR-SMP-3c-locks-1: cross-CPU wake proof — a BSP thread blocks; an AP job
 * sched_unblocks it (atomic CAS); it resumes on the BSP scheduler. Runs from
 * the (scheduled) FS phase thread — kmain's APIC section predates sched_init
 * (the first placement corrupted via a NULL current_thread; DDR D5). */
static int g_smp_have_aps;
static struct tcb *g_cw_thread;
static void crosswake_thread(void *arg) {
    (void)arg;
    kputs("[smp] cross-wake waiting\r\n");
    sched_block();
    kputs("[smp] cross-wake OK\r\n");
}
static void crosswake_job(void) {
    sched_unblock(g_cw_thread);
}
static void crosswake_proof(void) {
    if (!g_smp_have_aps)
        return;
    g_cw_thread = sched_create(crosswake_thread, 0, "crosswake");
    if (!g_cw_thread)
        return;
    uint64_t dl = g_ticks + 100;
    while (g_cw_thread->state != THREAD_BLOCKED && g_ticks < dl)
        yield();                                 /* let it run + block */
    /* Target a genuine AP (not the BSP): only APs run the mailbox-draining idle
     * loop, so a job posted to the BSP's slot would never run. This proof thread
     * is itself a kernel thread and may run on any CPU now (cap-2b), so we must
     * pick by is_bsp, NOT by "not the current CPU". */
    unsigned ap = 0;
    for (unsigned i = 0; i < lapic_cpu_count(); i++) {
        struct percpu *pc = percpu_get(i);
        if (pc && pc->present && !pc->is_bsp) { ap = i; break; }
    }
    smp_run_on(ap, crosswake_job);
    dl = g_ticks + 100;                          /* let the wake land + run */
    while (g_cw_thread->state == THREAD_BLOCKED && g_ticks < dl)
        yield();
}

/* cap-2b proof: a ring thread actually runs on a non-BSP CPU. Spawn several
 * READY *kernel* probes, kick the APs, and spin (NOT yielding, so this CPU does
 * not run them all) until a probe reports a cpu_idx other than ours. */
static volatile uint32_t g_probe_cpumask;
static void probe_thread(void *arg) {
    (void)arg;
    for (volatile int i = 0; i < 200000; i++)   /* bounded spin; sets our CPU's bit */
        __atomic_or_fetch(&g_probe_cpumask, 1u << (this_cpu()->cpu_idx), __ATOMIC_SEQ_CST);
}
static void smpsched_proof(void) {
    if (!g_smp_have_aps)
        return;
    for (int i = 0; i < 6; i++)
        sched_create(probe_thread, 0, "probe");
    smp_resched_all();                           /* wake idle APs to pick them up */
    uint32_t self_bit = 1u << this_cpu()->cpu_idx;
    uint64_t dl = g_ticks + 100;
    while (!(g_probe_cpumask & ~self_bit) && g_ticks < dl)
        __asm__ volatile("pause");
    kputs((g_probe_cpumask & ~self_bit) ? "[smp] sched cross-CPU OK\r\n"
                                        : "[smp] sched cross-CPU FAIL\r\n");
}

/* cap-3 proof: an AP's own LAPIC timer fires (preemption). Read a non-BSP CPU's
 * per-CPU tick counter, wait ~300 ms, and confirm it advanced — under cap-2b
 * (no AP timer) it would stay put. */
static void smppreempt_proof(void) {
    if (!g_smp_have_aps)
        return;
    /* Measure a genuine AP (is_bsp==0): the BSP's timer always ticked, so
     * picking it would not prove AP preemption. This thread may itself run on an
     * AP now (cap-2b), so select by is_bsp, not by "not the current CPU". */
    struct percpu *pc = 0;
    for (unsigned i = 0; i < lapic_cpu_count(); i++) {
        struct percpu *c = percpu_get(i);
        if (c && c->present && !c->is_bsp) { pc = c; break; }
    }
    if (!pc) {
        kputs("[smp] ap preempt FAIL\r\n");
        return;
    }
    uint64_t t0 = pc->ticks;
    uint64_t dl = g_ticks + 30;                  /* ~300 ms of BSP ticks */
    while (g_ticks < dl)
        __asm__ volatile("pause");
    kputs(pc->ticks > t0 ? "[smp] ap preempt OK\r\n" : "[smp] ap preempt FAIL\r\n");
}

/* cap-4 proof: a ring-3 thread runs on an AP. The FS phase has just spawned a
 * fleet of user processes; nudge the APs and poll the flag schedule() sets when
 * a non-BSP CPU claims a user thread. (The flag-then-print split keeps console
 * I/O out of the locked scheduler path.) */
static void smpuser_proof(void) {
    if (!g_smp_have_aps)
        return;
    smp_resched_all();
    uint64_t dl = g_ticks + 200;                 /* up to ~2 s — user procs are live */
    while (!g_user_on_ap && g_ticks < dl)
        yield();
    kputs(g_user_on_ap ? "[smp] user on AP OK\r\n" : "[smp] user on AP FAIL\r\n");
}

/* L6: SYS_SPAWN_AGENT hook. Loads the agent directly from its embedded kernel
 * bytes (NOT from SFS — that mount is gone by scheduler time) and marks the new
 * process CAP_AGENT so it is rate-limited + mem-capped (ADR-026). */
static uint32_t g_aether_daemon_pid;
static long aether_spawn_agent_hook(const char *task) {
    (void)task;
    struct tcb *ut = 0;
    uint64_t len = (uint64_t)(agent_base_elf_end - agent_base_elf);
    if (elf_load((void *)(uintptr_t)agent_base_elf, len, "AGENT", &ut) != ELF_OK || !ut)
        return -1;
    ut->is_agent = 1;                  /* authority BEFORE the first run */
    ut->parent_pid = g_aether_daemon_pid;
    sched_unblock(ut);                 /* elf_load returns the thread BLOCKED */
    return (long)ut->pid;
}

/* Place the execve target image on the FAT32 root (the process root for the
 * syscall layer, set by vfs_set_default_mnt below) so the ring-3 systest can
 * SYS_EXECVE("/EXECTEST.ELF"). The FAT32 disk is rebuilt fresh for every gate
 * run, so the file never pre-exists. */
static void fat_place_exec_image(cap_t cap, int mnt) {
    uint64_t elen = (uint64_t)(exectest_elf_end - exectest_elf);
    struct vfs_file ef;
    if (vfs_create(cap, mnt, "/EXECTEST.ELF", &ef) != 0 ||
        vfs_write(cap, &ef, 0, exectest_elf, (uint32_t)elen) != (int)elen)
        kputs("[exec] FAT32 placement of /EXECTEST.ELF failed\r\n");
    else
        kputs("[exec] placed /EXECTEST.ELF for the execve test\r\n");
}

/* Block device test: runs as a thread so virtio-blk I/O can block on its IRQ
 * (interrupt-driven completion, not busy-poll). */
static void blk_test_thread(void *arg) {
    (void)arg;
    if (blk_count() == 0) {
        kputs("[blk] no block device\r\n");
        return;
    }
    uint64_t buf = pmm_alloc_page();
    if (blk_read(0, 0, (void *)(uintptr_t)buf, 1) == 0) {
        uint16_t sig = *(volatile uint16_t *)(uintptr_t)(buf + 510);
        kputs("[blk] read sector 0, boot sig=");
        kputhex(sig);
        kputs(sig == 0xAA55 ? "  (MBR OK)\r\n" : "  (?)\r\n");
    } else {
        kputs("[blk] read sector 0 failed\r\n");
    }

    /* Write/read round-trip on a scratch sector in the boot disk's padding.
     * MUST be past the kernel's on-disk image: the kernel loads from LBA 17 and
     * the build caps it at 256 KiB (512 sectors), so it can occupy up to LBA
     * ~529. QEMU persists writes back to the image file, so writing into the
     * kernel region would corrupt the kernel for the next boot. LBA 1500 is past
     * the kernel and inside the 1 MiB (2048-sector) image. */
    uint64_t w = pmm_alloc_page(), r = pmm_alloc_page();
    for (int i = 0; i < 512; i++)
        ((volatile uint8_t *)(uintptr_t)w)[i] = (uint8_t)(i * 7 + 3);
    int wr = blk_write(0, 1500, (void *)(uintptr_t)w, 1);
    int rd = blk_read(0, 1500, (void *)(uintptr_t)r, 1);
    int ok = (wr == 0 && rd == 0);
    for (int i = 0; ok && i < 512; i++)
        if (((volatile uint8_t *)(uintptr_t)r)[i] != ((volatile uint8_t *)(uintptr_t)w)[i])
            ok = 0;
    kputs(ok ? "[blk] write/read round-trip OK\r\n" : "[blk] write/read round-trip FAILED\r\n");
}

/* List up to 32 entries of a directory by path (cap-gated). */
static void fs_list(cap_t cap, int mnt, const char *path) {
    char name[16];
    uint32_t sz;
    kputs("[fs] dir ");
    kputs(path);
    kputs(":\r\n");
    for (int i = 0; i < 32 && vfs_readdir(cap, mnt, path, i, name, &sz) == 0; i++) {
        kputs("    ");
        kputs(name);
        kputs("  ");
        kputdec(sz);
        kputs(" bytes\r\n");
    }
}

/* Open a file by path, read it, and echo its contents (cap-gated). */
static void fs_print_file(cap_t cap, int mnt, const char *path) {
    struct vfs_file f;
    if (vfs_open(cap, mnt, path, &f) != 0) {
        kputs("[fs] ");
        kputs(path);
        kputs(" not found\r\n");
        return;
    }
    uint64_t buf = pmm_alloc_page();
    uint32_t want = (f.size < 4095) ? (uint32_t)f.size : 4095;
    int n = vfs_read(cap, &f, 0, (void *)(uintptr_t)buf, want);
    ((char *)(uintptr_t)buf)[(n > 0) ? n : 0] = 0;
    kputs("[fs] ");
    kputs(path);
    kputs(": \"");
    kputs((char *)(uintptr_t)buf);
    kputs("\"\r\n");
}

/* Exercise the write path: create a file, write to it, read it back, and
 * create+delete a scratch file — all capability-gated (CAP_FS_WRITE). The
 * persistent file (/KOUT.TXT) is left on disk for host-side fsck validation. */
static void fs_write_test(cap_t cap, int mnt) {
    const char *msg = "kernel wrote this";
    uint32_t mlen = 0;
    while (msg[mlen]) mlen++;

    struct vfs_file f;
    if (vfs_create(cap, mnt, "/KOUT.TXT", &f) != 0) {
        kputs("[fs] create /KOUT.TXT failed\r\n");
    } else {
        int w = vfs_write(cap, &f, 0, msg, mlen);
        kputs("[fs] wrote /KOUT.TXT (");
        kputdec((w > 0) ? (uint64_t)w : 0);
        kputs(" bytes)\r\n");
        struct vfs_file g;
        if (vfs_open(cap, mnt, "/KOUT.TXT", &g) == 0) {
            uint64_t buf = pmm_alloc_page();
            int n = vfs_read(cap, &g, 0, (void *)(uintptr_t)buf, 4095);
            ((char *)(uintptr_t)buf)[(n > 0) ? n : 0] = 0;
            kputs("[fs] /KOUT.TXT readback: \"");
            kputs((char *)(uintptr_t)buf);
            kputs("\"\r\n");
        }
    }

    /* exercise create + delete on a scratch file */
    if (vfs_create(cap, mnt, "/TMP.TXT", &f) == 0) {
        vfs_write(cap, &f, 0, "temp", 4);
        if (vfs_unlink(cap, mnt, "/TMP.TXT") == 0)
            kputs("[fs] created+deleted /TMP.TXT OK\r\n");
    }
}

/* VFS/FAT32 test: mount, list directories, read files — including a nested
 * path through a subdirectory — all capability-gated. */
static void fs_test_thread(void *arg) {
    cap_t cap = (cap_t)(uintptr_t)arg;
    int mnt = -1, blk = -1;
    for (unsigned j = 0; j < blk_count(); j++) {
        int id = vfs_mount(j);
        if (id >= 0) { mnt = id; blk = (int)j; break; }
    }
    if (mnt < 0) {
        kputs("[fs] no mountable filesystem found\r\n");
        return;
    }
    /* 5b: this stable FAT32 mount is the process root for the syscall layer
     * (the SFS mount is later reformatted by the destructive self-tests). */
    vfs_set_default_mnt(mnt);
    kputs("[fs] mounted ");
    kputs(vfs_fs_name(mnt));
    kputs(" on blk");
    kputdec((uint64_t)blk);
    kputs(" (mnt ");
    kputdec((uint64_t)mnt);
    kputs(")\r\n");

    fs_list(cap, mnt, "/");
    fs_print_file(cap, mnt, "/HELLO.TXT");
    fs_list(cap, mnt, "/DOCS");
    fs_print_file(cap, mnt, "/DOCS/NOTE.TXT");
    fs_print_file(cap, mnt, "/LongFileName.txt");   /* VFAT long-name read (4j) */

    /* RTC wall-clock (4j): the kernel now has a real date for FS timestamps. */
    {
        struct rtc_time t;
        rtc_now(&t);
        kputs("[rtc] ");
        kputdec(t.year); kputs("-"); kputdec(t.month); kputs("-"); kputdec(t.day);
        kputs(" "); kputdec(t.hour); kputs(":"); kputdec(t.minute); kputs("\r\n");
    }
    fs_write_test(cap, mnt);
    fat_place_exec_image(cap, mnt);   /* 5b slice 7: /EXECTEST.ELF for systest's execve */

    /* SFS: format a blank disk in-kernel, mount it (FAT32 declines, SFS claims
     * it by superblock magic), then exercise the CoW B+tree — create 10 files,
     * look each up by name, and verify the inode numbers round-trip. */
    if (blk_count() > 2) {
        struct blk_device *sbd = blk_get(2);
        if (sbd && sfs_format(sbd) == 0) {
            int smnt = vfs_mount(2);
            if (smnt >= 0) {
                kputs("[sfs] mounted ");
                kputs(vfs_fs_name(smnt));
                kputs(" on blk2\r\n");

                uint32_t created[10];
                char nm[8] = "FILE0";
                int made = 0;
                for (int i = 0; i < 10; i++) {
                    nm[4] = (char)('0' + i);
                    struct vfs_file f;
                    if (vfs_create(cap, smnt, nm, &f) != 0)
                        break;
                    created[i] = f.cookie;     /* SFS inode number */
                    made++;
                }
                int verified = 0;
                for (int i = 0; i < made; i++) {
                    nm[4] = (char)('0' + i);
                    struct vfs_file f;
                    if (vfs_open(cap, smnt, nm, &f) == 0 && f.cookie == created[i])
                        verified++;
                }
                char rn[16];
                uint32_t rs;
                int dcount = 0;
                for (int i = 0; i < 32 && vfs_readdir(cap, smnt, "/", i, rn, &rs) == 0; i++)
                    dcount++;

                kputs("[sfs] created ");
                kputdec((uint64_t)made);
                kputs(", verified ");
                kputdec((uint64_t)verified);
                kputs(", dir entries ");
                kputdec((uint64_t)dcount);
                kputs((made == 10 && verified == 10 && dcount == 10)
                          ? " - create/lookup OK\r\n" : " - FAIL\r\n");

                /* Slice 4f: 64 KiB extent write -> read-back -> grow past EOF.
                 * Buffers (64 KiB each, order-4) intentionally leaked — one-shot
                 * self-test, matching the other demo threads. */
                uint64_t wbuf = pmm_alloc_pages(4);
                uint64_t rbuf = pmm_alloc_pages(4);
                if (wbuf && rbuf) {
                    uint8_t *w = (uint8_t *)(uintptr_t)wbuf;
                    for (int i = 0; i < 65536; i++) w[i] = (uint8_t)(i * 31 + 7);
                    int rw_ok = 0, grow_ok = 0;
                    struct vfs_file df;
                    if (vfs_create(cap, smnt, "DATA", &df) == 0 &&
                        vfs_write(cap, &df, 0, (void *)(uintptr_t)wbuf, 65536) == 65536) {
                        struct vfs_file rf;
                        if (vfs_open(cap, smnt, "DATA", &rf) == 0 &&
                            vfs_read(cap, &rf, 0, (void *)(uintptr_t)rbuf, 65536) == 65536 &&
                            memcmp((void *)(uintptr_t)wbuf, (void *)(uintptr_t)rbuf, 65536) == 0)
                            rw_ok = 1;
                        if (vfs_write(cap, &rf, 65536, (void *)(uintptr_t)wbuf, 4096) == 4096) {
                            struct vfs_file gf;
                            if (vfs_open(cap, smnt, "DATA", &gf) == 0 && gf.size == 65536 + 4096)
                                grow_ok = 1;
                        }
                    }
                    kputs("[sfs] 64K write/read ");
                    kputs(rw_ok ? "byte-exact OK" : "FAIL");
                    kputs(", grow ");
                    kputs(grow_ok ? "to 69632 OK\r\n" : "FAIL\r\n");
                }

                /* Phase 5a: write each embedded static ELF to SFS, read it BACK
                 * from SFS, and load it into a fresh W^X address space as a ring-3
                 * process (ADR-021). Done while SFS is still mounted, i.e. before
                 * the destructive journal/snapshot tests below.
                 *   1. hello   — prints "HELLO FROM RING-3", exits via sys_exit.
                 *   2. wxviol  — writes to its own RX text page; the W^X negative
                 *      regression. The kernel must turn that ring-3 #PF into a
                 *      clean process kill and keep running (the SFS self-tests
                 *      after this still pass, proving survival). */
                /* DDR-713 root-cause fix: register the SYS_SPAWN_AGENT hook BEFORE
                 * any user thread exists. The preemptive scheduler runs spawned
                 * threads while kmain is still booting (the remaining ELF loads +
                 * self-tests take seconds of virtio I/O), so a sovereign UI could
                 * otherwise call SYS_SPAWN_AGENT before a late-registered hook and
                 * get -ENOSYS. The hook needs only the embedded agent image;
                 * g_aether_daemon_pid (the agents' parent) is filled in when the
                 * daemon is loaded below — earlier spawns parent to 0 (reaper). */
                aether_set_spawn_hook(aether_spawn_agent_hook);
                crosswake_proof();   /* DDR-SMP-3c-locks-1: AP wakes a BSP thread
                                        (needs the live scheduler — this thread) */
                smpsched_proof();    /* ADR-031 cap-2b: a ring thread runs on an AP */
                smppreempt_proof();  /* ADR-031 cap-3: an AP's timer preempts */
                user_boot_from_sfs(cap, smnt, "HELLO.ELF", hello_elf, hello_elf_end, 0);
                kputs("[wx] spawning W^X violator (expect a clean user-kill)\r\n");
                user_boot_from_sfs(cap, smnt, "WXVIOL.ELF", wx_elf, wx_elf_end, 0);
                /* Phase 5b: the syscall test program (read/write/open/... grows
                 * per slice). Runs in ring 3 and prints SYS* sentinels. */
                user_boot_from_sfs(cap, smnt, "SYSTEST.ELF", systest_elf, systest_elf_end, 0);
                /* L7 (DDR-703): ring-3 keyboard reader. Polls SYS_INPUT_POLL;
                 * the smoke-input gate injects keys via QEMU sendkey. */
                user_boot_from_sfs(cap, smnt, "INPUTTST.ELF", inputtest_elf, inputtest_elf_end, 0);
                /* L7 (DDR-706): a client window — creates + commits a surface that
                 * the compositor composites onto the desktop. Exercised by smoke-surface. */
                user_boot_from_sfs(cap, smnt, "SURFTEST.ELF", surfacetest_elf, surfacetest_elf_end, 0);
                /* L7 (DDR-704): the in-house compositor, spawned with CAP_SOVEREIGN
                 * so it may flip the mode via SYS_SET_MODE. With a GPU it renders
                 * the sovereign desktop and reacts to the keyboard; without one it
                 * exits via SYS_FB_INFO -> -ENODEV. Exercised by smoke-compositor. */
                user_boot_from_sfs(cap, smnt, "COMPOSIT.ELF",
                                   compositor_elf, compositor_elf_end,
                                   1 /* CAP_SOVEREIGN before first run */);
                /* PROC-D step 1: SET_TLS thread pointer + WRITEV gather-write.
                 * Prints "PRADYOS_TLS_OK WRITEV_OK" on success. */
                user_boot_from_sfs(cap, smnt, "TLSTEST.ELF", tlstest_elf, tlstest_elf_end, 0);
                /* PROC-D step 3: the first ring-3 C program, statically linked
                 * against musl; its crt/__libc_start_main set up TLS + stdio and
                 * printf flushes via SYS_WRITEV. Prints "PRADYOS_MUSL_OK ...". */
                user_boot_from_sfs(cap, smnt, "CMUSL.ELF", cmusl_elf, cmusl_elf_end, 0);
                /* 5d: two concurrent FPU users sharing XMM0. Each survives only
                 * if the context switch saves/restores FPU state (ADR-023 §D8).
                 * Both print "PRADYOS_FPU_OK"; either prints FAIL on clobber. */
                user_boot_from_sfs(cap, smnt, "FPUTST1.ELF", fputest_elf, fputest_elf_end, 0);
                user_boot_from_sfs(cap, smnt, "FPUTST2.ELF", fputest_elf, fputest_elf_end, 0);
                /* 5d: pradyos-init becomes PID 1 — orphans reparent to it and it
                 * reaps the tree forever. It forks a child that exits 42; init
                 * collects it and logs "init: reaped PID=N exit=42". */
                struct tcb *it = user_boot_from_sfs(cap, smnt, "INIT.ELF",
                                                    init_elf, init_elf_end, 0);
                if (it)
                    sched_set_init_pid(it->pid);
                /* 5e: launch the PRISM shell as init's child (execve-based
                 * respawn is deferred — ADR-024 §D5). It reads commands from the
                 * console; init reaps it on exit. */
                struct tcb *pr = user_boot_from_sfs(cap, smnt, "PRISM.ELF",
                                                    prism_elf, prism_elf_end, 0);
                if (pr && it)
                    pr->parent_pid = it->pid;

                /* L6: AETHER daemon as init's child, granted CAP_SOVEREIGN (it
                 * owns mode + approve authority). Loaded now while SFS is mounted;
                 * it auto-spawns the test agent once the scheduler runs. */
                struct tcb *dm = user_boot_from_sfs(cap, smnt, "AETHERD.ELF",
                                                    aether_daemon_elf, aether_daemon_elf_end,
                                                    1 /* CAP_SOVEREIGN before first run */);
                if (dm) {
                    if (it) dm->parent_pid = it->pid;
                    g_aether_daemon_pid = dm->pid;
                }
                smpuser_proof();     /* ADR-031 cap-4: ring 3 runs on an AP */
                /* DDR-714C3: plenty of disk I/O has completed by now (the SFS
                 * ELF loads above) — assert a blk completion ran off the BSP. */
                if (g_smp_have_aps)
                    kputs(virtio_blk_completed_on_ap()
                              ? "[blk] msix on AP OK\r\n"
                              : "[blk] msix on AP FAIL\r\n");

                /* Slice 4g: journal abort/commit/crash-replay (destructive —
                 * reformats the disk, so release the VFS mount first). */
                vfs_unmount(smnt);
                int jr = sfs_selftest_journal(sbd);
                kputs("[sfs] journal ");
                kputs(jr == 7 ? "abort/commit/replay OK\r\n" : "FAIL\r\n");

                /* Slice 4h: snapshot version isolation (destructive). */
                int sr = sfs_selftest_snapshot(sbd);
                kputs("[sfs] snapshot ");
                kputs(sr == 3 ? "version-isolation OK\r\n" : "FAIL\r\n");

                /* Slice 4i: inline LZ4 + metadata tags (destructive). */
                int lr = sfs_selftest_lz4(sbd);
                kputs("[sfs] lz4+tags ");
                kputs(lr == 7 ? "compress/readback/tag OK\r\n" : "FAIL\r\n");
            } else {
                kputs("[sfs] mount failed\r\n");
            }
        } else {
            kputs("[sfs] format failed\r\n");
        }
    }

    /* ext4 read-only (slice 4j): mount the 4th disk and read a host-written file. */
    if (blk_count() > 3) {
        int emnt = vfs_mount(3);
        if (emnt >= 0) {
            struct vfs_file ef;
            if (vfs_open(cap, emnt, "/EXT4.TXT", &ef) == 0) {
                uint64_t buf = pmm_alloc_page();
                uint32_t want = (ef.size < 4095) ? (uint32_t)ef.size : 4095;
                int n = vfs_read(cap, &ef, 0, (void *)(uintptr_t)buf, want);
                ((char *)(uintptr_t)buf)[(n > 0) ? n : 0] = 0;
                kputs("[ext4] mounted ");
                kputs(vfs_fs_name(emnt));
                kputs("; /EXT4.TXT: \"");
                kputs((char *)(uintptr_t)buf);
                kputs("\"\r\n");
            } else {
                kputs("[ext4] open /EXT4.TXT failed\r\n");
            }
        } else {
            kputs("[ext4] mount failed\r\n");
        }
    }
}

static void sched_demo(void) {
    uint64_t tsc_hz = calibrate_tsc_hz();
    kputs("NEXUS: TSC ~");
    kputdec(tsc_hz / 1000000);
    kputs(" MHz\r\n");

    sched_init();
    sched_create(bench_partner, 0, "bench");
    bench_ctx_switch(tsc_hz);
    bench_done = 1;                           /* let the bench thread retire */

    /* Create the demo threads with interrupts masked so each thread's capability
     * (->arg) is fully set before the timer can schedule it. */
    __asm__ volatile("cli");
    ipc_demo();
    ring_demo();
    bus_demo();
    sched_create(blk_test_thread, 0, "blk");
    sched_start_reaper();                     /* 5b-9: reclaim orphaned zombie procs */
    struct tcb *fst = sched_create(fs_test_thread, 0, "fs");
    if (fst)
        fst->arg = (void *)(uintptr_t)cap_create(fst->caps, RES_FILE, FS_RES_ID,
                                                 CAP_FS_READ | CAP_FS_WRITE |
                                                 CAP_FS_SFS_READ | CAP_FS_SFS_ADMIN);
    __asm__ volatile("sti");
    kputs("NEXUS: scheduler + IPC + ring-3 + virtio-blk + VFS live\r\n");

    for (;;)                               /* this context is now the idle thread */
        __asm__ volatile("hlt");
}

/* Phase 5b slice 2: exercise the validated user-pointer copy path (ADR-022).
 * Builds a throwaway user address space with one RW and one read-only user page,
 * switches to it, and drives copyin/copyout/copyinstr. The two negative cases (a
 * wild pointer and a write to a read-only page) MUST return -EFAULT with the
 * kernel surviving — i.e. no #PF at CPL 0, no panic. Runs with interrupts masked
 * around the CR3 switch so a timer tick can't schedule on the throwaway AS. */
static void uaccess_selftest(void) {
    kputs("NEXUS: uaccess (copyin/copyout/copyinstr) tests\r\n");

    uint64_t save_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(save_cr3));

    uint64_t as = vmm_new_address_space();
    if (!as) { kputs("[uaccess] no address space\r\n"); return; }

    const uint64_t UVA_RW = VMM_USER_MIN;            /* a writable user page     */
    const uint64_t UVA_RO = VMM_USER_MIN + 0x1000;   /* a read-only user page    */
    void *frw = ptnode_alloc();
    void *fro = ptnode_alloc();
    if (!frw || !fro) {
        kputs("[uaccess] no frame\r\n");
        vmm_destroy_address_space(as);
        return;
    }

    const char *probe = "uaccess-probe";
    uint64_t plen = 0;
    while (probe[plen]) plen++;                       /* 13 chars, excl. NUL      */
    memcpy((void *)(uintptr_t)frw, probe, (size_t)plen + 1);  /* seed via identity view */

    vmm_map_in(as, UVA_RW, (uint64_t)(uintptr_t)frw, VMM_USER | VMM_RW | VMM_NX);
    vmm_map_in(as, UVA_RO, (uint64_t)(uintptr_t)fro, VMM_USER | VMM_NX);  /* no RW */

    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    __asm__ volatile("mov %0, %%cr3" :: "r"(as) : "memory");

    char kbuf[32];
    /* Test 1: copyin from a good user page. */
    ssize_t r1 = copyin(kbuf, (const void __user *)(uintptr_t)UVA_RW, (size_t)plen + 1);
    int t1 = (r1 == (ssize_t)(plen + 1)) && (memcmp(kbuf, probe, (size_t)plen + 1) == 0);

    /* Test 2: copyin from an unmapped user address -> EFAULT, kernel survives. */
    ssize_t r2 = copyin(kbuf, (const void __user *)(uintptr_t)0xdeadbeef000ull, 8);
    int t2 = (r2 == -EFAULT);

    /* Test 3: copyout to a read-only user page -> EFAULT (W^X upheld). */
    ssize_t r3 = copyout((void __user *)(uintptr_t)UVA_RO, probe, (size_t)plen + 1);
    int t3 = (r3 == -EFAULT);

    /* Test 4: copyinstr on a valid user string -> string + correct length. */
    char sbuf[32];
    size_t slen = 0;
    ssize_t r4 = copyinstr(sbuf, (const void __user *)(uintptr_t)UVA_RW, sizeof sbuf, &slen);
    int t4 = (r4 == (ssize_t)plen) && (slen == plen) && (memcmp(sbuf, probe, (size_t)plen) == 0);

    __asm__ volatile("mov %0, %%cr3" :: "r"(save_cr3) : "memory");
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");

    kputs(t1 ? "[uaccess] copyin good page OK\r\n"      : "[uaccess] copyin good page FAIL\r\n");
    kputs(t2 ? "[uaccess] copyin bad ptr EFAULT OK\r\n" : "[uaccess] copyin bad ptr FAIL\r\n");
    kputs(t3 ? "[uaccess] copyout RO page EFAULT OK\r\n": "[uaccess] copyout RO page FAIL\r\n");
    kputs(t4 ? "[uaccess] copyinstr OK\r\n"             : "[uaccess] copyinstr FAIL\r\n");

    vmm_destroy_address_space(as);   /* frees the AS + both data frames (leaf pages) */
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

/* IMP-D self-test: build a one-page user AS, COW-fork it, fault the child's
 * copy, and prove the parent's page is untouched (true copy-on-write isolation).
 * Exercises vmm_fork_address_space_cow + vmm_cow_fault + the PMM refcount path
 * directly (the real ring-3 #PF path is covered by smoke-sysfork/syswait). */
static void cow_selftest(void) {
    uint64_t parent = vmm_new_address_space();
    if (!parent) { kputs("[vmm] COW fork FAIL (no AS)\r\n"); return; }
    void *pf = ptnode_alloc();
    if (!pf) { vmm_destroy_address_space(parent); kputs("[vmm] COW fork FAIL (no frame)\r\n"); return; }

    uint64_t va = 0x8000000000ull;                  /* user range (PML4 slot 1) */
    *(volatile uint64_t *)pf = 0xAAAAAAAAAAAAAAAAull;
    vmm_map_in(parent, va, (uint64_t)(uintptr_t)pf, VMM_USER | VMM_RW | VMM_NX);

    uint64_t child = vmm_fork_address_space_cow(parent);
    int ok = 0;
    if (child) {
        int rc = vmm_cow_fault(child, va);          /* simulate the child writing */
        uint64_t cphys = vmm_resolve(child, va);
        uint64_t pphys = vmm_resolve(parent, va);
        if (rc == 0 && cphys && cphys != pphys) {
            *(volatile uint64_t *)(uintptr_t)cphys = 0xBBBBBBBBBBBBBBBBull;
            ok = (*(volatile uint64_t *)(uintptr_t)pphys == 0xAAAAAAAAAAAAAAAAull) &&
                 (*(volatile uint64_t *)(uintptr_t)cphys == 0xBBBBBBBBBBBBBBBBull);
        }
        vmm_destroy_address_space(child);
    }
    vmm_destroy_address_space(parent);
    kputs(ok ? "[vmm] COW fork copy-on-write OK\r\n" : "[vmm] COW fork FAIL\r\n");
}

void kmain(struct boot_info *bi) {
    kputs("NEXUS: entered kmain (64-bit long mode, ring 0)\r\n");

    print_boot_info(bi);

    gdt_init();
    kputs("NEXUS: kernel GDT loaded\r\n");

    /* DDR-SMP-3b: current_thread + the SYSCALL kstack are %gs-relative — claim
     * the BSP percpu slot NOW, after gdt_init (whose gs selector reload zeroes
     * the base) and before anything schedules. */
    percpu_init_early();

    idt_init();
    kputs("NEXUS: IDT loaded (48 vectors: 32 exceptions + 16 IRQ)\r\n");

    cpu_mitigations_init();              /* IMP-A: IBRS/STIBP/SSBD/IBPB where available */
    cpu_enable_sse();                    /* PROC-D: x87+SSE for ring-3 C (musl) — ADR-023 §D8 */

    tss_init_cpu(0, 0);                  /* BSP is cpu_idx 0 here; rsp0 set per user thread */
    syscall_init();                      /* EFER.SCE + STAR/LSTAR/SFMASK + dispatch */
    vmm_init();                          /* record kernel master CR3 + enable EFER.NXE (W^X) */
    kputs("NEXUS: TSS loaded, SYSCALL/SYSRET armed, NX enabled\r\n");

    kvga_line("NEXUS KERNEL OK", 1);
    kputs("NEXUS KERNEL OK\r\n");

    /* Self-test: a breakpoint must be caught by the IDT and resume execution. */
    kputs("NEXUS: IDT self-test — executing int3...\r\n");
    __asm__ volatile("int3");
    kputs("NEXUS: resumed after int3 — exception handling verified\r\n");

    /* Hardware interrupts: PIC + PIT, then enable and watch the clock tick. */
    pic_remap();
    pit_init(100);                       /* 100 Hz */
    console_rx_init();                   /* 5e: arm COM1 RX IRQ + ring buffer */
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
    uaccess_selftest();                  /* Phase 5b: validated user-pointer copy path */

    vdso_init();                         /* IMP-C: shared clock page (PIT advances it) */
    cow_selftest();                      /* IMP-D: copy-on-write fork isolation */

    /* Phase 3: hardware discovery + first device driver. */
    acpi_init();
    /* DDR-714 stage A: LAPIC up + APIC timer takes the 100 Hz tick (PIT is then
     * masked). Device IRQs stay on the 8259. Needs ACPI (MADT) + a live PIT for
     * calibration, both true here; falls back to the PIT if no MADT. */
    if (lapic_init() == 0) {
        lapic_timer_100hz();
        percpu_init_bsp();               /* ADR-030 stage 2: BSP identity first */
        struct percpu *pc = this_cpu();
        kputs("[percpu] bsp idx=");
        kputdec(pc ? pc->cpu_idx : 999);
        kputs(" id=");
        kputdec(pc ? pc->apic_id : 999);
        kputs("\r\n");
        smp_start_aps();                 /* ADR-029: INIT-SIPI; APs idle for jobs */
        /* DDR-SMP-3c-alpha: prove cross-CPU dispatch — one job per AP. */
        unsigned jobs = 0;
        for (unsigned i = 0; i < lapic_cpu_count(); i++) {
            if (i == this_cpu()->cpu_idx)
                continue;
            if (smp_run_on(i, smp_test_job) != 0)
                continue;
            uint64_t deadline = g_ticks + 100;          /* <= 1 s each */
            while (!smp_job_done(i) && g_ticks < deadline)
                __asm__ volatile("pause");
            if (smp_job_done(i))
                jobs++;
        }
        kputs("[smp] jobs done=");
        kputdec(jobs);
        kputs("\r\n");
        g_smp_have_aps = (jobs > 0);     /* cross-wake proof runs later, once
                                            the scheduler is up (DDR D5) */
    }
    pcie_init();
    for (unsigned i = 0; i < pcie_device_count(); i++) {
        const struct pcie_device *d = pcie_device_get(i);
        if (d->vendor_id == 0x1AF4 && d->class_code == 0x01)    /* virtio storage */
            virtio_blk_init(d->bus, d->dev, d->func);
        if (d->vendor_id == 0x1AF4 && d->class_code == 0x02)    /* virtio network (NET-A) */
            virtio_net_init(d->bus, d->dev, d->func);
        if (d->vendor_id == 0x1AF4 && d->class_code == 0x03)    /* virtio GPU (L7 slice 0) */
            virtio_gpu_init(d->bus, d->dev, d->func);
        if (d->vendor_id == 0x1AF4 && d->class_code == 0x09)    /* virtio input/pointer (DDR-705) */
            virtio_input_init(d->bus, d->dev, d->func);
    }
    net_init();                          /* NET-B: bring up lwIP over virtio-net */
    aether_init();                       /* Layer 6: PMM-pool queue + audit rings */
    aether_selftest();                   /* Layer 6: smoke-aether-queue (PRADYOS_AETHER_QUEUE_OK) */
    aether_sectest();                    /* Layer 6: smoke-aether-sec (bounds + clean-kill paths) */
    fat32_register();                    /* Phase 4: register the FS driver with the VFS */
    sfs_register();                      /* Phase 4: SOVEREIGN FS (ADR-018) */
    ext4_register();                     /* Phase 4j: ext4 read-only compat */

    kputs("NEXUS: starting scheduler\r\n");
    sched_demo();                          /* never returns (becomes the idle thread) */
}
