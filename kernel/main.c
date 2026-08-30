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
#include "ioapic.h"        /* DDR-874: I/O APIC + GSI routing */
#include "ahci.h"          /* DDR-875: AHCI SATA */
#include "e1000e.h"        /* DDR-876: Intel e1000e NIC */
#include "numa.h"          /* DDR-882: SRAT NUMA topology */
#include "pdrive.h"        /* DDR-890: PRADYOS Drive workspace FS */
#include "pstate.h"        /* DDR-892: CPU frequency scaling */
#include "smp.h"
#include "percpu.h"
#include "pcie.h"
#include "blk.h"
#include "virtio_blk.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include "virtio_net.h"
#include "nvme.h"
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
#include "metric_page.h"  /* F#68/DDR-795: sealed objective root page */
#include "fwcfg.h"        /* DDR-804: per-boot probe selection */
#include "rng.h"          /* DDR-816: kernel entropy */
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
    /* DDR-961: three-way now that the wait is bounded. -1 is still "cap denied";
     * -ETIMEDOUT is new and must not be folded into it -- a bounded wait that
     * hides its own expiry is worse than an unbounded one. The timeout prints a
     * string no gate asserts on, so an expiry turns the affected gate red rather
     * than passing quietly. */
    int rr = ipc_recv(current_thread->caps, cap, &demo_ep, buf);
    if (rr == 0) {
        kputs("[recv] received: ");
        kputhex(buf[0]);
        kputs(" ");
        kputhex(buf[1]);
        kputs("\r\n");
    } else if (rr == -ETIMEDOUT) {
        kputs("[recv] TIMEOUT\r\n");
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
    /* DDR-964: BLOCKED until ->arg holds the capability. sched_create() is
     * already on a run queue when it returns, so another CPU's rq_steal can
     * enter the thread before the mint below and read an unwritten ->arg. */
    struct tcb *recv = sched_create_blocked(ipc_receiver_thread, 0, "recv");
    struct tcb *send = sched_create_blocked(ipc_sender_thread, 0, "send");
    if (!recv || !send) {
        kputs("NEXUS: ipc_demo — thread create failed\r\n");
        if (recv) sched_destroy(recv);   /* never ran: still BLOCKED */
        if (send) sched_destroy(send);
        return;
    }
    /* Mint each thread a single, resource-bound capability with only its right. */
    recv->arg = (void *)(uintptr_t)cap_create(recv->caps, RES_IPC, DEMO_EP_ID, CAP_IPC_RECV);
    send->arg = (void *)(uintptr_t)cap_create(send->caps, RES_IPC, DEMO_EP_ID, CAP_IPC_SEND);
    sched_unblock(recv);
    sched_unblock(send);
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
    /* DDR-964: BLOCKED until ->arg holds the capability (see ipc_demo). */
    struct tcb *prod = sched_create_blocked(ring_producer_thread, 0, "prod");
    struct tcb *cons = sched_create_blocked(ring_consumer_thread, 0, "cons");
    if (!prod || !cons) {
        kputs("NEXUS: ring_demo — thread create failed\r\n");
        if (prod) sched_destroy(prod);   /* never ran: still BLOCKED */
        if (cons) sched_destroy(cons);
        return;
    }
    prod->arg = (void *)(uintptr_t)cap_create(prod->caps, RES_IPC, RING_EP_ID, CAP_IPC_SEND);
    cons->arg = (void *)(uintptr_t)cap_create(cons->caps, RES_IPC, RING_EP_ID, CAP_IPC_RECV);
    sched_unblock(prod);
    sched_unblock(cons);
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
        if (bcast_wait(&sub_a, &e) == -ETIMEDOUT) {   /* DDR-961 */
            kputs("[sub-approve] TIMEOUT\r\n");
            return;                                   /* e is UNFILLED — do not read it */
        }
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
    if (bcast_wait(&sub_b, &e) == -ETIMEDOUT) {       /* DDR-961 */
        kputs("[sub-alert] TIMEOUT\r\n");
        return;                                       /* e is UNFILLED — do not read it */
    }
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
    /* DDR-964: BLOCKED until ->arg holds the capability (see ipc_demo). The
     * publisher is unblocked LAST so both subscribers are registered before it
     * can broadcast. */
    struct tcb *a = sched_create_blocked(sub_approvals_thread, 0, "sub-approve");
    struct tcb *b = sched_create_blocked(sub_alerts_thread, 0, "sub-alert");
    struct tcb *p = sched_create_blocked(publisher_thread, 0, "pub");
    if (!a || !b || !p) {
        kputs("NEXUS: bus_demo — thread create failed\r\n");
        if (a) sched_destroy(a);         /* never ran: still BLOCKED */
        if (b) sched_destroy(b);
        if (p) sched_destroy(p);
        return;
    }
    a->arg = (void *)(uintptr_t)cap_create(a->caps, RES_IPC, BUS_EP_ID, CAP_IPC_RECV);
    b->arg = (void *)(uintptr_t)cap_create(b->caps, RES_IPC, BUS_EP_ID, CAP_IPC_RECV);
    p->arg = (void *)(uintptr_t)cap_create(p->caps, RES_IPC, BUS_EP_ID, CAP_BROADCAST);
    sched_unblock(a);
    sched_unblock(b);
    sched_unblock(p);
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
extern const unsigned char surfdestroytest_elf[];    /* L7: surface lifecycle/destroy test (DDR-729) */
extern const unsigned char surfdestroytest_elf_end[];
extern const unsigned char agentmetricstest_elf[];   /* L7: per-agent live metrics probe (DDR-730) */
extern const unsigned char agentmetricstest_elf_end[];
extern const unsigned char capnettest_elf[];         /* L6/7: CAP_NET socket-authority probe (DDR-731) */
extern const unsigned char capnettest_elf_end[];
extern const unsigned char rootmounttest_elf[];      /* fs: per-process root-mount probe (DDR-739) */
extern const unsigned char rootmounttest_elf_end[];
extern const unsigned char benchtest_elf[];            /* perf: RDTSC benchmark (DDR-870) */
extern const unsigned char benchtest_elf_end[];
extern const unsigned char stackdemand_elf[];         /* ADR-038: demand-paged stack probe */
extern const unsigned char stackdemand_elf_end[];
extern const unsigned char ftrunctest_elf[];          /* fs: ftruncate probe (DDR-866) */
extern const unsigned char ftrunctest_elf_end[];
extern const unsigned char renametest_elf[];          /* fs: SFS rename probe (DDR-962) */
extern const unsigned char renametest_elf_end[];
extern const unsigned char fsrmtest_elf[];            /* fs: ring-3 file lifecycle probe (DDR-744) */
extern const unsigned char fsrmtest_elf_end[];
extern const unsigned char fat32mctest_elf[];         /* DDR-973: FAT32 multi-cluster read probe */
extern const unsigned char fat32mctest_elf_end[];
extern const unsigned char nethammer_elf[];           /* DDR-990: two-CPU connect/close hammer */
extern const unsigned char nethammer_elf_end[];
int ps2kbd_selftest(void);                           /* DDR-993 §3: paired-modifier kernel arm */

/* DDR-994 §6 arm B: spins in the REAL mnt_lock against a held scratch mount,
 * so the [yieldstall] line it emits proves the detector is wired to the call
 * site rather than merely callable. Exits once the arming thread releases. */
static void yieldstall_waiter_thread(void *arg) {
    (void)arg;
    vfs_yieldstall_wait();
    kputs("PRADYOS_YIELDSTALL_WAITER_DONE\r\n");
    sched_exit(0);
}
extern const unsigned char modkeystest_elf[];         /* DDR-991: modifier / extended-key probe */
extern const unsigned char modkeystest_elf_end[];
extern const unsigned char egressaudittest_elf[];     /* DDR-801: per-destination egress audit */
extern const unsigned char egressaudittest_elf_end[];
extern const unsigned char sovegresstest_elf[];       /* DDR-800: sovereign-egress audit */
extern const unsigned char sovegresstest_elf_end[];
extern const unsigned char hkdftest_elf[];            /* DDR-818: HKDF vectors */
extern const unsigned char hkdftest_elf_end[];
extern const unsigned char x25519test_elf[];          /* DDR-820: X25519 vectors */
extern const unsigned char x25519test_elf_end[];
extern const unsigned char sha512test_elf[];          /* DDR-821: SHA-512 vectors */
extern const unsigned char sha512test_elf_end[];
extern const unsigned char aeadtest_elf[];            /* DDR-819: AEAD vectors */
extern const unsigned char aeadtest_elf_end[];
extern const unsigned char ed25519test_elf[];         /* DDR-821: Ed25519 vectors */
extern const unsigned char ed25519test_elf_end[];
extern const unsigned char acctest_elf[];             /* DDR-813: ACC gate */
extern const unsigned char acctest_elf_end[];
extern const unsigned char lockboxtest_elf[];         /* DDR-812: metric lockbox */
extern const unsigned char lockboxtest_elf_end[];
extern const unsigned char coderewritetest_elf[];     /* DDR-842: code-rewrite gate */
extern const unsigned char coderewritetest_elf_end[];
extern const unsigned char invarianttest_elf[];       /* DDR-844: S1-S8 attack gate */
extern const unsigned char invarianttest_elf_end[];
extern const unsigned char auditchaintest_elf[];      /* DDR-842: audit chain gate */
extern const unsigned char auditchaintest_elf_end[];
extern const unsigned char actiondagtest_elf[];       /* DDR-839: DAG action queue */
extern const unsigned char actiondagtest_elf_end[];
extern const unsigned char actionreadtest_elf[];      /* DDR-1015: 3C ACTION_READ_FILE */
extern const unsigned char actionreadtest_elf_end[];
extern const unsigned char spawndepthtest_elf[];      /* DDR-838: spawn-depth cap */
extern const unsigned char spawndepthtest_elf_end[];
extern const unsigned char ckpttest_elf[];            /* DDR-837: checkpoint/resume */
extern const unsigned char ckpttest_elf_end[];
extern const unsigned char agentmemtest_elf[];        /* DDR-836: agent memory */
extern const unsigned char agentmemtest_elf_end[];
extern const unsigned char vaulttest_elf[];           /* DDR-834: credential vault */
extern const unsigned char vaulttest_elf_end[];
extern const unsigned char accrottest_elf[];          /* DDR-815: ACC rotation */
extern const unsigned char accrottest_elf_end[];
extern const unsigned char agstest_elf[];             /* DDR-814: AGS goal signing */
extern const unsigned char agstest_elf_end[];
extern const unsigned char sha256test_elf[];          /* DDR-811: SHA-256 vectors */
extern const unsigned char sha256test_elf_end[];
extern const unsigned char sigpipetest_elf[];         /* DDR-805: SIGPIPE probe */
extern const unsigned char sigpipetest_elf_end[];
extern const unsigned char privacynettest_elf[];      /* DDR-802: privacy netfilter */
extern const unsigned char privacynettest_elf_end[];
extern const unsigned char rtcmonotest_elf[];         /* DDR-796: SYS_CLOCK monotonicity */
extern const unsigned char rtcmonotest_elf_end[];
extern const unsigned char metrictest_elf[];          /* F#68/DDR-795: metric-region probe */
extern const unsigned char metrictest_elf_end[];
extern const unsigned char sysinfotest_elf[];         /* sys: SYS_SYSINFO probe (DDR-748) */
extern const unsigned char sysinfotest_elf_end[];
extern const unsigned char timetest_elf[];            /* sys: SYS_TIME probe (DDR-749) */
extern const unsigned char timetest_elf_end[];
extern const unsigned char dmesgtest_elf[];           /* sys: SYS_DMESG probe (DDR-750) */
extern const unsigned char dmesgtest_elf_end[];
extern const unsigned char killtest_elf[];            /* proc: SYS_KILL probe (DDR-755) */
extern const unsigned char killtest_elf_end[];
extern const unsigned char setnametest_elf[];         /* proc: SYS_SETNAME probe (DDR-756) */
extern const unsigned char setnametest_elf_end[];
extern const unsigned char syscallfuzz_elf[];         /* sec: syscall fuzz probe (DDR-758) */
extern const unsigned char syscallfuzz_elf_end[];
extern const unsigned char sfsroottest_elf[];         /* fs: persistent SFS-root probe (DDR-760) */
extern const unsigned char sfsroottest_elf_end[];
extern const unsigned char bigwritetest_elf[];        /* fs: ring-3 large-write probe (DDR-764) */
extern const unsigned char bigwritetest_elf_end[];
void aether_set_spawn_hook(long (*fn)(const char *task));  /* kernel/syscall/sys_aether.c */
void net_init(void);                             /* NET-B: lwip-port/pradyos_net.h */
void aether_init(void);                          /* Layer 6: kernel/aether/aether.c */
void aether_selftest(void);
void aether_sectest(void);
int  ramdisk_init(unsigned order);    /* DDR-972: memory-backed root for the ISO */
/* DDR-842: probe-gated audit-chain fault injection. main.c declares aether
 * entry points locally rather than including aether.h (see the three above). */
void aether_audit_tamper(void);

/* Write an embedded ELF to SFS, read it BACK from SFS, and load it as a ring-3
 * process. Genuinely exercises the filesystem load path (the bytes elf_load
 * parses come from sfs_read, not the embedded image). */
/* `sovereign` grants CAP_SOVEREIGN BEFORE the thread's first run: elf_load now
 * returns the thread BLOCKED, and the unblock happens only after the authority
 * flags are set (DDR-boot-authority-race). */
/* Unchanged entry point for the ~25 callers that keep the default root. */
static struct tcb *user_boot_from_sfs(cap_t cap, int smnt, const char *fname,
                               const unsigned char *elf, const unsigned char *elf_end,
                               int sovereign);

/* DDR-957: `root_mnt >= 0` selects the new thread's root mount BEFORE it is
 * unblocked. That ordering is the whole point -- this helper unblocks
 * internally, so a caller assigning ->root_mnt on the returned pointer would
 * race a thread that is already runnable (see main.c:1831). */
static struct tcb *user_boot_from_sfs_rooted(cap_t cap, int smnt, const char *fname,
                               const unsigned char *elf, const unsigned char *elf_end,
                               int sovereign, int root_mnt) {
    /* ITEM 47 / B#3 instrument (DDR-886): name every SFS-backed load as it is
     * ENTERED, so a boot that stops mid-block says WHICH load it stopped on.
     * fs_test_thread has been observed reaching stamp A and never stamp C while
     * every already-spawned thread keeps running and printing — i.e. it BLOCKS,
     * it does not crash. Each call below does vfs_create + vfs_write + vfs_read
     * against SFS, so a missed virtio-blk completion under -smp 4 would park it
     * here forever. Without this print the ~440-line window between the last
     * spawn and stamp C cannot be narrowed from a serial log alone. */
    { uint64_t blfl = console_line_lock();       /* DDR-963 §5 */
      kputs("[boot-load] "); kputs(fname); kputs(" t="); kputdec(g_ticks); kputs("\r\n");
      console_line_unlock(blfl); }
    uint64_t elen = (uint64_t)((uintptr_t)elf_end - (uintptr_t)elf);
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
    if (!buf) {
        /* DDR-941: was a SILENT return 0. ~30 probes boot through this one
         * function, so an unnamed failure is indistinguishable from a probe
         * that booted fine and then misbehaved — the exact ambiguity that made
         * smoke-wmorder's "never shrank" unreadable. */
        kputs("[boot-load] FAILED ");
        kputs(fname);
        kputs(" reason=no-256k-buf\r\n");
        return 0;
    }
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
        if (root_mnt >= 0)
            ut->root_mnt = root_mnt;   /* DDR-957: root BEFORE the first run */
        sched_unblock(ut);             /* elf_load returns the thread BLOCKED */
        kputs("[user] ELF loaded from SFS; ring-3 thread spawned\r\n");
        return ut;
    }
    /* DDR-941: name the file. "ELF load FAILED rc=8" with ~30 probes booting
     * through this function does not say WHICH probe never started, so a gate
     * that later reports a missing behaviour cannot tell "its probe failed to
     * load" from "its probe loaded and then failed". */
    kputs("[boot-load] FAILED ");
    kputs(fname);
    kputs(" reason=elf rc=");
    kputdec((uint64_t)(-lr));
    /* DDR-945: free frames AT THE FAILURE, not 500 ticks later. The [hb] sample
     * cannot answer this: elf_load's ELF_E_NOMEM paths call
     * vmm_destroy_address_space() on the way out, returning up to 2047 frames,
     * so the next heartbeat shows the RECOVERED count and a transient
     * exhaustion is invisible. Reading pmmfree here is the only way to tell
     * "no frames were available" from "frames were available and something
     * else failed". */
    kputs(" pmmfree=");
    kputdec(pmm_free_page_count());
    kputs("\r\n");
    return 0;
}

static struct tcb *user_boot_from_sfs(cap_t cap, int smnt, const char *fname,
                               const unsigned char *elf, const unsigned char *elf_end,
                               int sovereign) {
    return user_boot_from_sfs_rooted(cap, smnt, fname, elf, elf_end, sovereign, -1);
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
    /* DDR-939: same spawn check as rqstress_proof — a cross-CPU FAIL must not
     * be read as a scheduling defect when no probe thread was ever created. */
    int probe_spawned = 0;
    for (int i = 0; i < 6; i++)
        if (sched_create(probe_thread, 0, "probe"))
            probe_spawned++;
    smp_resched_all();                           /* wake idle APs to pick them up */
    uint32_t self_bit = 1u << this_cpu()->cpu_idx;
    uint64_t dl = g_ticks + 100;
    while (!(g_probe_cpumask & ~self_bit) && g_ticks < dl)
        __asm__ volatile("pause");
    if (g_probe_cpumask & ~self_bit) {
        kputs("[smp] sched cross-CPU OK\r\n");
    } else {
        kputs("[smp] sched cross-CPU FAIL spawned=");   /* DDR-939 */
        kputdec((uint64_t)probe_spawned);
        kputs("/6\r\n");
    }
}

/* DDR-SMP-rq-1 proof: a thread storm across the per-CPU runqueues — 24
 * kernel threads in 3 waves, spread by steal + wake; ALL must complete. */
static volatile uint32_t g_rqs_done;
static void rqstress_worker(void *arg) {
    (void)arg;
    for (volatile int i = 0; i < 50000; i++)
        ;
    __atomic_add_fetch(&g_rqs_done, 1, __ATOMIC_SEQ_CST);
}
static void rqstress_proof(void) {
    /* DDR-939: count successful spawns. `n=0` alone cannot distinguish 24
     * threads that were created and never scheduled (the DDR-936 class) from
     * sched_create returning NULL 24 times — 24 x (TCB + 16 KiB stack) is
     * ~384 KiB of kernel heap requested late in boot. Those are different
     * subsystems. Same check DDR-934 added to the blk probes. */
    int rqs_spawned = 0;
    for (int wave = 0; wave < 3; wave++) {
        for (int i = 0; i < 8; i++)
            if (sched_create(rqstress_worker, 0, "rqs"))
                rqs_spawned++;
        smp_resched_all();
        uint64_t dl = g_ticks + 100;
        while (g_rqs_done < (uint32_t)((wave + 1) * 8) && g_ticks < dl)
            yield();
    }
    /* DDR-885: the per-wave deadlines above are PACING (they keep the waves
     * overlapping, which is the point of the stress) — they must not decide the
     * verdict. Previously the last wave's 100-tick deadline could expire with
     * workers still runnable, and `g_rqs_done == 24` was read immediately,
     * reporting FAIL for threads that were LATE, not lost. Because DDR-785 fails
     * any gate whose boot contains a foreign probe FAIL, that one late wave
     * reddened unrelated gates across several CI shards (run 31803482520 failed
     * smoke-sfs-gc on a docs-only commit).
     *
     * Drain budget = 300 ticks, the same total the three waves already had, so
     * the worst case stays bounded (S2) at ~600 ticks / 6 s — well inside every
     * consuming gate's TIMEOUT_S. The assertion is unchanged: all 24 must land. */
    uint64_t drain_dl = g_ticks + 300;
    while (g_rqs_done < 24 && g_ticks < drain_dl)
        yield();
    if (g_rqs_done == 24) {
        kputs("[smp] rqstress OK\r\n");
    } else {
        /* Report the count — a bare FAIL cannot distinguish "23 landed late"
         * from "the runqueues lost a thread". */
        kputs("[smp] rqstress FAIL n=");
        kputdec(g_rqs_done);
        /* DDR-939: spawned<24 => allocation failure, and the DDR-936 scheduler
         * hunt does not apply to this instance. spawned=24 with n=0 => the
         * threads exist and none ran, which makes this the loudest reproducer
         * of the DDR-936 class (24 threads, and ubcas=/ubrq= are already live). */
        kputs(" spawned=");
        kputdec((uint64_t)rqs_spawned);
        kputs("/24\r\n");
    }
}

/* DDR-BLK-1 proof: two threads keep requests in flight on ONE disk
 * concurrently (each reads its own sector 8x and must round-trip cleanly).
 * Bits 0/1 = reader ok; bits 8/9 = reader error. */
static volatile uint32_t g_mq_done;
static void blkmq_reader(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    struct blk_device *bd = blk_get(0);
    uint64_t buf = pmm_alloc_page();
    int ok = (bd && buf);
    for (int i = 0; ok && i < 8; i++)
        if (bd->read(bd, id, (void *)(uintptr_t)buf, 1) != 0)
            ok = 0;
    if (buf)
        pmm_free_page(buf);
    __atomic_or_fetch(&g_mq_done, 1u << (ok ? id : id + 8), __ATOMIC_SEQ_CST);
}
static void blkmq_proof(void) {
    /* DDR-934: sched_create returns NULL if the TCB or the 16 KiB stack cannot
     * be allocated. Discarding that made "never created" indistinguishable from
     * "created but never ran" — both surface only as done=0x0. */
    unsigned mq_spawned = 0;
    if (sched_create(blkmq_reader, (void *)0, "mq0")) mq_spawned++;
    if (sched_create(blkmq_reader, (void *)1, "mq1")) mq_spawned++;
    /* DDR-966: kick the APs, matching rqstress_proof. sched_create enqueues on
     * the CALLING CPU's run queue, so another CPU only collects a fresh worker
     * when it next runs its scheduler — and an idle AP sits in hlt until its
     * own timer tick. Without this the BSP burns the deadline below in yield()
     * while runnable workers wait on halted APs, which is exactly the captured
     * `done=0x0 spawned=2/2`: created, never run. */
    smp_resched_all();
    uint64_t dl = g_ticks + 200;
    while ((g_mq_done & 3u) != 3u && !(g_mq_done >> 8) && g_ticks < dl)
        yield();
    /* DDR-886: the loop above is PACING — it must not decide the verdict. A
     * reader that has not landed yet was previously indistinguishable from one
     * that failed its read. Drain with the same 200-tick budget the pacing loop
     * had (worst case 4 s at 100 Hz, far inside this gate's TIMEOUT_S), then
     * report the bitmask: low bit unset = LATE, high bit set = read FAILED. */
    uint64_t drain = g_ticks + 200;
    while ((g_mq_done & 3u) != 3u && !(g_mq_done >> 8) && g_ticks < drain)
        yield();
    if ((g_mq_done & 3u) == 3u) {
        kputs("[blk] multi-inflight OK\r\n");
    } else {
        kputs("[blk] multi-inflight FAIL done=");
        kputhex(g_mq_done);
        kputs(" spawned=");                 /* DDR-934 */
        kputdec(mq_spawned);
        kputs("/2\r\n");
    }
}

/* DDR-759 (M1 SMP audit): concurrent block-read DATA integrity. Unlike
 * blkmq_proof (which only checks read success), each worker verifies the bytes
 * it read match a single-threaded reference checksum for its sector — so a
 * completion mis-routed to the wrong DDR-BLK-1 slot (wrong data) is detected.
 * 4 workers under -smp 4 distribute across CPUs, keeping the 8 slots busy and
 * routing completions to APs (DDR-714C3). */
static volatile uint32_t g_blkint_done;         /* bits 0..3 done, 8..11 error   */
static uint32_t g_blkint_ref[4];                 /* per-sector reference checksum */

static uint32_t blk_sum(const uint8_t *b) {
    uint32_t s = 0;
    for (int i = 0; i < 512; i++)               /* 512-byte sectors (blk.h) */
        s = s * 31u + b[i];
    return s;
}

/* DDR-898: per-worker progress. g_blkint_done is written only at the very END of
 * the worker, so done=0x0 (observed in CI 31907631454) proves "no worker
 * RETURNED" but cannot tell "never scheduled" from "ran and stalled mid-loop".
 * These counters make that distinction directly. */
static volatile uint32_t g_blkint_prog[4];

static void blkint_worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    unsigned sec = id & 3u;
    struct blk_device *bd = blk_get(0);
    uint64_t buf = pmm_alloc_page();
    int ok = (bd && buf);
    for (int i = 0; ok && i < 64; i++) {
        if (bd->read(bd, sec, (void *)(uintptr_t)buf, 1) != 0) { ok = 0; break; }
        if (blk_sum((const uint8_t *)(uintptr_t)buf) != g_blkint_ref[sec]) { ok = 0; break; }
        if (id < 4)
            __atomic_add_fetch(&g_blkint_prog[id], 1, __ATOMIC_RELAXED);
    }
    if (buf)
        pmm_free_page(buf);
    __atomic_or_fetch(&g_blkint_done, 1u << (ok ? id : id + 8), __ATOMIC_SEQ_CST);
}

static void smp_blk_integrity(void) {
    struct blk_device *bd = blk_get(0);
    uint64_t buf = pmm_alloc_page();
    /* DDR-886: three sites emitted the identical "blk integrity FAIL" text, so a
     * resource shortage could not be told from a data mismatch. Each now names
     * itself. */
    if (!bd || !buf) { if (buf) pmm_free_page(buf); kputs("[smp] blk integrity FAIL no-device-or-page\r\n"); return; }
    /* Single-threaded reference: sectors 0..3 are stable read-only boot image. */
    int refok = 1;
    for (unsigned sec = 0; sec < 4; sec++) {
        if (bd->read(bd, sec, (void *)(uintptr_t)buf, 1) != 0) { refok = 0; break; }
        g_blkint_ref[sec] = blk_sum((const uint8_t *)(uintptr_t)buf);
    }
    pmm_free_page(buf);
    if (!refok) { kputs("[smp] blk integrity FAIL reference-read\r\n"); return; }

    /* DDR-934: see blkmq_proof — a discarded sched_create return hides an
     * allocation failure behind the same done=0x0 that a scheduling stall
     * produces. Four workers cost ~4 x 16 KiB of kernel stack, late in boot. */
    unsigned bi_spawned = 0;
    if (sched_create(blkint_worker, (void *)0, "bi0")) bi_spawned++;
    if (sched_create(blkint_worker, (void *)1, "bi1")) bi_spawned++;
    if (sched_create(blkint_worker, (void *)2, "bi2")) bi_spawned++;
    if (sched_create(blkint_worker, (void *)3, "bi3")) bi_spawned++;
    smp_resched_all();                      /* DDR-966: see blkmq_proof */
    uint64_t dl = g_ticks + 400;
    while ((g_blkint_done & 0xfu) != 0xfu && g_ticks < dl)
        yield();
    /* DDR-886: drain before the verdict, same 400-tick budget the pacing loop
     * had (worst case 8 s at 100 Hz). Then report the bitmask so LATE (a low
     * bit unset) is distinguishable from WRONG (a high bit set) — previously
     * both printed the same word, which is why an intermittent CI failure here
     * carried no information. */
    uint64_t drain = g_ticks + 400;
    while ((g_blkint_done & 0xfu) != 0xfu && g_ticks < drain)
        yield();
    if ((g_blkint_done & 0xfu) == 0xfu && !(g_blkint_done >> 8)) {
        kputs("[smp] blk integrity OK\r\n");
    } else {
        kputs("[smp] blk integrity FAIL ");
        kputs((g_blkint_done >> 8) ? "checksum-mismatch" : "workers-late");
        kputs(" done=");
        kputhex(g_blkint_done);
        kputs(" spawned=");                 /* DDR-934 */
        kputdec(bi_spawned);
        kputs("/4");
        /* DDR-930: per-worker completed iterations (of 64). prog=0,0,0,0 means
         * the workers never ran at all (scheduling), any non-zero means they
         * ran and stalled — done= alone cannot tell those apart. */
        kputs(" prog=");
        for (int pi = 0; pi < 4; pi++) {
            if (pi) kputs(",");
            kputdec(g_blkint_prog[pi]);
        }
        kputs("\r\n");
    }
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

/* rq-3 proof: a cross-CPU unblock kicks an IDLE AP (directed wake IPI) so it
 * steals the thread promptly. Let the APs settle to idle, block a probe, then
 * unblock it from here — expect g_resched_ipis to advance AND the probe to run. */
static volatile int g_rp_ran;
static struct tcb *g_rp_thread;
static void resched_probe(void *arg) {
    (void)arg;
    sched_block();
    g_rp_ran = 1;
}
static void smpresched_proof(void) {
    if (!g_smp_have_aps)
        return;
    uint64_t dl = g_ticks + 20;                  /* let APs drain work -> idle */
    while (g_ticks < dl)
        yield();
    uint64_t before = g_resched_ipis;
    g_rp_thread = sched_create(resched_probe, 0, "resched");
    dl = g_ticks + 50;
    while (g_rp_thread && g_rp_thread->state != THREAD_BLOCKED && g_ticks < dl)
        yield();

    /* DDR-1004: ESTABLISH the precondition instead of assuming the settle loop
     * produced it.
     *
     * sched_unblock kicks a CPU only `if (c != self && o && o->present &&
     * o->idle)` (sched.c:1825) -- and says so: "the timer remains the backstop
     * if none is (visibly) idle". The IPI is best-effort BY DESIGN. The 20-tick
     * settle above was only ever a hope that some AP would be idle by now; it
     * never checked. When none was, zero IPIs were sent, the thread still ran on
     * the timer backstop, and this proof printed FAIL on a correct system.
     *
     * That is DDR-883's finding recurring. DDR-883 narrowed the IPI term to
     * cpu_count > 2, but the real precondition is "an idle non-self CPU was
     * visible at unblock", not a CPU count -- so the false failure survived at
     * 4 CPUs, where it is rare enough to look like a flake. Measured: 1 CI
     * failure in 3 runs on tip 30a19ce (`ipis=0 ran=1`, shard 8), 0 in 20 local
     * runs on the same kernel 60b35c96d70253f5.
     *
     * So: wait for an idle non-self CPU, and record whether we ever saw one.
     * This is deliberately NOT a way to make the arm always pass -- see the
     * SKIP branch below, which refuses to report OK when the IPI path was not
     * exercised. */
    int idle_seen = 0;
    dl = g_ticks + 50;
    while (g_ticks < dl && !idle_seen) {
        struct percpu *self_pc = this_cpu();
        int self_idx = self_pc ? (int)self_pc->cpu_idx : 0;
        for (int c = 0; c < PERCPU_MAX; c++) {
            struct percpu *o = percpu_get((uint32_t)c);
            /* DDR-1014: `!o->is_bsp` MATCHES THE KERNEL. smp_resched_one
             * declines to kick the BSP, so a BSP counted as an idle candidate
             * here made the proof expect an IPI the kernel would never send --
             * `ipis=0 ran=1 idle=1`, a FAIL on a correct system. That is the
             * artefact this fix has: CI run on 72a474a, shard 5, reddening
             * smoke-percpu-sched (which does not even own the assertion --
             * `resched FAIL` is a GLOBAL_FORBIDDEN entry, DDR-791).
             *
             * DDR-1004 §6.1 predicted a residual here and named the wrong one:
             * it described a timing race (a CPU leaving idle between the sample
             * and the call). That window is real but narrow. THIS one needs no
             * timing at all -- it is a predicate mismatch, and it fires whenever
             * the BSP is the idle CPU. The two loops must ask the same question
             * or the proof is not testing the kernel, it is testing a paraphrase
             * of it. */
            if (c != self_idx && o && o->present && o->idle && !o->is_bsp) {
                idle_seen = 1;
                break;
            }
        }
        if (!idle_seen)
            yield();
    }
    sched_unblock(g_rp_thread);                  /* enqueue here + kick an idle AP */
    dl = g_ticks + 50;
    while (!g_rp_ran && g_ticks < dl)
        yield();
    /* DDR-883. The property that must hold is that the unblocked thread RUNS.
     * Requiring an IPI on top of that was over-specified: at -smp 2 there is one
     * AP, the thread is picked up without a cross-CPU kick, and the proof
     * reported FAIL with ipis=0 ran=1 — a correct system failing its own test,
     * deterministically, 5/5. No gate had ever run -smp 2.
     *
     * The IPI term is kept where it is genuinely expected — more than one AP,
     * i.e. cpu_count > 2 — so the cross-CPU kick keeps its coverage at the 3-
     * and 4-CPU configurations every existing SMP gate uses. Both terms are
     * printed on failure because "never ran" and "no IPI needed" demand
     * opposite actions.
     */
    int ipi_expected = (lapic_cpu_count() > 2);
    /* DDR-1004: three outcomes, not two.
     *
     * The IPI is only owed when an idle non-self CPU was actually visible. If
     * none ever was, this boot did not exercise the rq-3 path at all, and both
     * OK and FAIL would be lies -- OK would silently stop testing the kick
     * (the vacuity trap DDR-973 §6 and DDR-996 each caught once), FAIL would
     * blame the scheduler for a precondition the harness failed to create.
     *
     * SKIP carries neither "OK" nor "FAIL", so it trips no gate sentinel and no
     * GLOBAL_FORBIDDEN entry, and a run of them is visible in the log as the
     * coverage gap it is.
     *
     * NOTE the residual race, stated rather than hidden: `idle_seen` is sampled
     * just before sched_unblock, and a CPU can leave idle in between. That
     * window is far narrower than the old unconditional assertion, but it is
     * not zero -- so a FAIL with idle=1 is strong evidence and not yet proof. */
    if (g_rp_ran && ipi_expected && !idle_seen && g_resched_ipis == before) {
        kputs("[smp] resched SKIP no-idle-ap ran=1\r\n");
    } else if (g_rp_ran && (!ipi_expected || g_resched_ipis > before)) {
        kputs("[smp] resched OK\r\n");
    } else {
        kputs("[smp] resched FAIL ipis=");
        kputdec(g_resched_ipis - before);
        kputs(" ran=");
        kputdec((uint64_t)g_rp_ran);
        kputs(" idle=");                  /* DDR-1004: was the IPI even owed? */
        kputdec((uint64_t)idle_seen);
        kputs("\r\n");
    }
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
    /* DDR-777: entry marker. Its ABSENCE in a failing run means the !g_smp_have_aps
     * guard above returned silently (APs never came up); its presence with no
     * OK/FAIL below means we stalled inside the poll — and the [hb] heartbeat then
     * says whether g_ticks was still advancing (timer stall vs. never resuming
     * from yield()). */
    kputs("[smp] user-on-AP probe t="); kputdec(g_ticks); kputs("\r\n");
    while (!g_user_on_ap && g_ticks < dl)
        yield();
    kputs(g_user_on_ap ? "[smp] user on AP OK t=" : "[smp] user on AP FAIL t=");
    kputdec(g_ticks); kputs("\r\n");
}

/* L6: SYS_SPAWN_AGENT hook. Loads the agent directly from its embedded kernel
 * bytes (NOT from SFS — that mount is gone by scheduler time) and marks the new
 * process CAP_AGENT so it is rate-limited + mem-capped (ADR-026). */
static uint32_t g_aether_daemon_pid;
static long aether_spawn_agent_hook(const char *task) {
    (void)task;
    struct tcb *ut = 0;
    uint64_t len = (uint64_t)((uintptr_t)agent_base_elf_end - (uintptr_t)agent_base_elf);
    if (elf_load((void *)(uintptr_t)agent_base_elf, len, "AGENT", &ut) != ELF_OK || !ut)
        return -1;
    ut->is_agent = 1;                  /* authority BEFORE the first run */
    ut->is_net = 1;                    /* DDR-731: agents are the sanctioned socket users
                                        * (live mode talks to Ollama over SYS_SOCK_*) */
    ut->parent_pid = g_aether_daemon_pid;
    sched_unblock(ut);                 /* elf_load returns the thread BLOCKED */
    return (long)ut->pid;
}

/* Place the execve target image on the FAT32 root (the process root for the
 * syscall layer, set by vfs_set_default_mnt below) so the ring-3 systest can
 * SYS_EXECVE("/EXECTEST.ELF"). The FAT32 disk is rebuilt fresh for every gate
 * run, so the file never pre-exists. */
static void fat_place_exec_image(cap_t cap, int mnt) {
    uint64_t elen = (uint64_t)((uintptr_t)exectest_elf_end - (uintptr_t)exectest_elf);
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

    /* DDR-831: last sector of the 2 MiB (4096-sector) boot image. Past any
     * kernel that fits in the image by construction; `make image` enforces it. */
#define BLK_SCRATCH_LBA 4095u
    /* Write/read round-trip on a scratch sector in the boot disk's padding.
     *
     * DDR-831: this MUST stay past the kernel's on-disk extent. QEMU persists
     * writes back to the image file, so a write inside the kernel region corrupts
     * the kernel — including the probe ELFs incbin'd into .rodata — for every
     * subsequent boot. That is exactly what OPEN-11 was: this test wrote to a
     * hardcoded LBA 1500 chosen when the kernel was capped at 512 sectors; after
     * DDR-827 raised the cap the kernel reached LBA ~1666 and the self-test began
     * writing into it.
     *
     * The scratch sector is now the LAST sector of the image, which is past any
     * kernel that fits in the image by construction, and `make image` enforces
     * that the kernel cannot reach it. Do not replace this with a literal. */
    uint64_t w = pmm_alloc_page(), r = pmm_alloc_page();
    for (int i = 0; i < 512; i++)
        ((volatile uint8_t *)(uintptr_t)w)[i] = (uint8_t)(i * 7 + 3);
    int wr = blk_write(0, BLK_SCRATCH_LBA, (void *)(uintptr_t)w, 1);
    int rd = blk_read(0, BLK_SCRATCH_LBA, (void *)(uintptr_t)r, 1);
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

    /* ADR-032: FS write-budget token-bucket proof (deterministic — no wall-clock
     * loop). (a) A fully DRAINED bucket, after simulated elapsed ticks, refills
     * and permits a write — impossible under the old LIFETIME cap (a spent budget
     * never returned). (b) A drained bucket with NO elapsed ticks still rejects —
     * the rate limit holds (anti-flood). We simulate elapsed time by rewinding
     * fs_budget_tick; the lazy refill in vfs_write does the rest. */
    {
        uint64_t bp = pmm_alloc_page();              /* 4 KiB */
        if (bp) {
            memset((void *)(uintptr_t)bp, 0xB6, 4096);
            struct vfs_file bf;
            int ok_refill = 0, ok_limit = 0;
            if (vfs_create(cap, mnt, "/BUDGET.TMP", &bf) == 0) {
                current_thread->fs_write_budget = 0;                 /* spent */
                current_thread->fs_budget_tick  =
                    (g_ticks > 8) ? g_ticks - 8 : 0;                 /* 8 ticks elapsed */
                ok_refill = (vfs_write(cap, &bf, 0, (void *)(uintptr_t)bp, 4096) == 4096);

                current_thread->fs_write_budget = 0;                 /* spent again */
                current_thread->fs_budget_tick  = g_ticks + 1000000; /* no time can have
                                                        elapsed past a far-future mark →
                                                        no refill (race-free) */
                ok_limit = (vfs_write(cap, &bf, 0, (void *)(uintptr_t)bp, 4096) < 0);
                vfs_unlink(cap, mnt, "/BUDGET.TMP");
            }
            pmm_free_page(bp);
            kputs((ok_refill && ok_limit) ? "PRADYOS_FS_BUDGET_OK\r\n"
                                          : "PRADYOS_FS_BUDGET_FAIL\r\n");
        }
    }

    /* DDR-739: mount ext4 (blk3) ONCE, early, if present — read-only and never
     * unmounted, so it survives to scheduler time (unlike SFS). Reused below by
     * both the ext4 read self-test AND the per-process root-mount probe. */
    int ext4_mnt = (blk_count() > 3) ? vfs_mount(3) : -1;

    /* DDR-770: mount id of a pre-provisioned SFS root (a host mkfs.sfs image
     * carrying a real /etc/aether/config), or -1. Set by the peek below and used
     * further down to root the AETHER daemon there instead of the kernel
     * reformatting+provisioning blk2 (DDR-760/761). */
    int prov_mnt = -1;

    /* DDR-768: cross-reboot persistence proof. A host mkfs.sfs image (attached
     * LAST via QEMU_SFS2/QEMU_SFSROOT) is peeked at the highest blk index — block
     * 0 for the SFS magic (the blank in-kernel SFS disk isn't formatted until
     * below, so this only fires for a PRE-formatted host image) — then mounted
     * and read back, proving the kernel decodes a host-authored SFS volume. */
    if (blk_count() >= 1) {
        unsigned pi = blk_count() - 1;
        uint8_t sb0[512];
        uint64_t magic = 0;
        if (blk_get(pi) && blk_read(pi, 0, sb0, 1) == 0) {
            memcpy(&magic, sb0, sizeof magic);
            if (magic == SFS_MAGIC) {
                int pmnt = vfs_mount(pi);
                struct vfs_file pf;
                if (pmnt >= 0 && vfs_open(cap, pmnt, "/PERSIST.TXT", &pf) == 0) {
                    static const char mark[] = "PRADYOS-SFS-PERSIST-DDR768-OK";
                    char pbuf[64];
                    int n = vfs_read(cap, &pf, 0, pbuf, sizeof pbuf - 1);
                    int ok = (n == (int)(sizeof mark - 1));
                    for (int i = 0; ok && i < n; i++)
                        if (pbuf[i] != mark[i]) ok = 0;
                    kputs(ok ? "PRADYOS_SFS_PERSIST_OK\r\n"
                             : "PRADYOS_SFS_PERSIST_FAIL\r\n");
                }
                /* DDR-769: nested-dir provisioning — read a host-authored file at
                 * a nested path, proving the kernel traverses mkfs.sfs-built
                 * directory hierarchies. Uses /etc/test/config (distinct from the
                 * real /etc/aether/config, which DDR-770 reserves for rooting). */
                if (pmnt >= 0 && vfs_open(cap, pmnt, "/etc/test/config", &pf) == 0) {
                    static const char nmark[] = "PRADYOS-SFS-NESTED-DDR769-OK";
                    char nbuf[64];
                    int n = vfs_read(cap, &pf, 0, nbuf, sizeof nbuf - 1);
                    int ok = (n == (int)(sizeof nmark - 1));
                    for (int i = 0; ok && i < n; i++)
                        if (nbuf[i] != nmark[i]) ok = 0;
                    kputs(ok ? "PRADYOS_SFS_NESTED_OK\r\n"
                             : "PRADYOS_SFS_NESTED_FAIL\r\n");
                }
                /* DDR-770: if this host image carries a real /etc/aether/config
                 * (a valid policy starts with "mode="), remember its mount so the
                 * AETHER daemon can root here — no kernel format/provision. */
                if (pmnt >= 0 && vfs_open(cap, pmnt, "/etc/aether/config", &pf) == 0) {
                    char cbuf[8];
                    int n = vfs_read(cap, &pf, 0, cbuf, 5);
                    if (n == 5 && cbuf[0]=='m' && cbuf[1]=='o' && cbuf[2]=='d' &&
                        cbuf[3]=='e' && cbuf[4]=='=')
                        prov_mnt = pmnt;                /* keep this mount */
                }
            }
        }
    }

    /* SFS: format a blank disk in-kernel, mount it (FAT32 declines, SFS claims
     * it by superblock magic), then exercise the CoW B+tree — create 10 files,
     * look each up by name, and verify the inode numbers round-trip. */
    if (blk_count() > 2) {
        struct blk_device *sbd = blk_get(2);
        if (sbd && sfs_format(sbd) == 0) {
            int smnt = vfs_mount(2);
            /* DDR-967: pids of the ring-3 probes rooted at smnt (fsrm,
             * ftruncate, rename-sfs). The destructive self-test block further
             * down REFORMATS this volume, so it waits on these before
             * umounting. 0 = that probe was never spawned. Locals, not a
             * writable global (DDR-826). */
            uint32_t smnt_pid[3] = { 0, 0, 0 };
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

                /* DDR-738: hierarchical directories — create/read a deep path,
                 * re-open it from a fresh walk, reject a dir-as-file and a
                 * missing intermediate, and enumerate each level. */
                {
                    static const char CFGTEXT[] = "mode=sovereign\n";
                    int ok = 1;
                    struct vfs_file f;
                    /* (1) mkdir -p /etc/aether + create the file, write+read back. */
                    char rb[32];
                    if (vfs_create(cap, smnt, "/etc/aether/config", &f) == 0 &&
                        vfs_write(cap, &f, 0, (void *)CFGTEXT, sizeof CFGTEXT - 1)
                            == (int)(sizeof CFGTEXT - 1)) {
                        struct vfs_file g;   /* (2) fresh walk resolves the same file */
                        if (vfs_open(cap, smnt, "/etc/aether/config", &g) == 0 &&
                            g.size == sizeof CFGTEXT - 1 &&
                            vfs_read(cap, &g, 0, rb, sizeof CFGTEXT - 1) == (int)(sizeof CFGTEXT - 1) &&
                            memcmp(rb, CFGTEXT, sizeof CFGTEXT - 1) == 0)
                            ; else ok = 0;
                    } else ok = 0;
                    /* (3) negative: a directory is not openable as a file, and a
                     * missing intermediate fails. */
                    struct vfs_file nf;
                    if (vfs_open(cap, smnt, "/etc", &nf) == 0) ok = 0;
                    if (vfs_open(cap, smnt, "/etc/nope/x", &nf) == 0) ok = 0;
                    /* (4) enumerate each level. */
                    char nm[256]; uint32_t sz;
                    int saw_aether = 0, saw_config = 0;
                    for (int i = 0; vfs_readdir(cap, smnt, "/etc", i, nm, &sz) == 0; i++)
                        if (nm[0]=='a'&&nm[1]=='e'&&nm[2]=='t'&&nm[3]=='h'&&nm[4]=='e'&&nm[5]=='r'&&!nm[6]) saw_aether = 1;
                    for (int i = 0; vfs_readdir(cap, smnt, "/etc/aether", i, nm, &sz) == 0; i++)
                        if (nm[0]=='c'&&nm[1]=='o'&&nm[2]=='n'&&nm[3]=='f'&&nm[4]=='i'&&nm[5]=='g'&&!nm[6]) saw_config = 1;
                    if (!saw_aether || !saw_config) ok = 0;
                    kputs(ok ? "[sfs] hier dirs OK\r\n" : "[sfs] hier dirs FAIL\r\n");
                }

                /* DDR-741: unlink (files) + rmdir (empty dirs) via tombstones. */
                {
                    /* DDR-984: this probe used to collapse twelve assertions into
                     * one `ok` flag and print a single word, so a failure said
                     * only THAT it broke, never WHICH step or with what rc. That
                     * is the DDR-824 lesson (the op= line naming the broken
                     * operation contains none of the forbidden string, so it was
                     * never printed) applied to a second probe — and it cost a
                     * real CI capture: shard 4 on 0480657 reddened smoke-percpu
                     * via GLOBAL_FORBIDDEN with nothing to diagnose, and 6/6
                     * local re-runs did not reproduce it.
                     *
                     * Record the first failing step and its return code instead.
                     * FAILURE PATH ONLY: the healthy boot still prints the same
                     * single OK line, so no gate parser changes and no extra
                     * steady-state output. */
                    int ok = 1, badstep = 0; long badrc = 0;
                    struct vfs_file f;
                    long rc;
#define SFS_UNLINK_STEP(n, expr, want)                                        \
                    do { rc = (long)(expr);                                   \
                         if (ok && !(want)) { ok = 0; badstep = (n); badrc = rc; } \
                    } while (0)
                    /* (1) create -> unlink -> gone from open + readdir. */
                    SFS_UNLINK_STEP(1, vfs_create(cap, smnt, "/A.TXT", &f), rc == 0);
                    SFS_UNLINK_STEP(2, vfs_unlink(cap, smnt, "/A.TXT"), rc == 0);
                    SFS_UNLINK_STEP(3, vfs_open(cap, smnt, "/A.TXT", &f), rc != 0);  /* must be gone */
                    /* (2) re-create the tombstoned name. */
                    SFS_UNLINK_STEP(4, vfs_create(cap, smnt, "/A.TXT", &f), rc == 0);
                    SFS_UNLINK_STEP(5, vfs_open(cap, smnt, "/A.TXT", &f), rc == 0);  /* back */
                    /* (3) rmdir requires empty: /D/E/F -> /D not empty; leaf-first. */
                    SFS_UNLINK_STEP(6, vfs_create(cap, smnt, "/D/E/F", &f), rc == 0);
                    SFS_UNLINK_STEP(7, vfs_unlink(cap, smnt, "/D"), rc != 0);        /* ENOTEMPTY */
                    SFS_UNLINK_STEP(8, vfs_unlink(cap, smnt, "/D/E/F"), rc == 0);
                    SFS_UNLINK_STEP(9, vfs_unlink(cap, smnt, "/D/E"), rc == 0);
                    SFS_UNLINK_STEP(10, vfs_unlink(cap, smnt, "/D"), rc == 0);
                    char nm[256]; uint32_t sz; int saw_d = 0;
                    for (int i = 0; vfs_readdir(cap, smnt, "/", i, nm, &sz) == 0; i++)
                        if (nm[0]=='D'&&!nm[1]) saw_d = 1;
                    SFS_UNLINK_STEP(11, saw_d, rc == 0);                  /* /D gone from readdir */
                    /* (4) absent name. */
                    SFS_UNLINK_STEP(12, vfs_unlink(cap, smnt, "/NOPE"), rc != 0);
#undef SFS_UNLINK_STEP
                    if (ok) {
                        kputs("[sfs] unlink/rmdir OK\r\n");
                    } else {
                        /* step= names WHICH assertion; rc= is what it returned.
                         * Printed BEFORE the summary line the gates match on, so
                         * GLOBAL_FORBIDDEN's 40-lines-of-context window carries
                         * it (DDR-824). */
                        kputs("[sfs] unlink/rmdir detail step="); kputdec((uint64_t)badstep);
                        kputs(" rc="); kputdec((uint64_t)badrc); kputs("\r\n");
                        kputs("[sfs] unlink/rmdir FAIL\r\n");
                    }
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
                smpresched_proof();  /* DDR-SMP-rq-3: unblock kicks an idle AP */
                /* DDR-734: egress-allowlist match/deny self-test. Install one
                 * test rule directly (kernel-side; ring 3 uses SYS_NET_ALLOW)
                 * and prove exact-match allow + wrong-host/port deny. The rule
                 * is a REAL entry that stays installed — harmless (192.0.2.1 is
                 * TEST-NET-1, RFC 5737; nothing routes there in the guest). */
                {
                    int netallow_check(uint32_t host_be, uint16_t port);
                    int netallow_add(uint32_t host_be, uint16_t port);
                    int ok = (netallow_add(0xC0000201u, 9999) == 0)          /* 192.0.2.1:9999 */
                          && (netallow_check(0xC0000201u, 9999) == 0)        /* exact match    */
                          && (netallow_check(0xC0000201u, 9998) != 0)        /* wrong port     */
                          && (netallow_check(0xC0000202u, 9999) != 0);       /* wrong host     */
                    kputs(ok ? "[net] allowlist OK\r\n" : "[net] allowlist FAIL\r\n");
                }
                /* DDR-806 settling measurement (stamp A). Pure instrumentation:
                 * two prints, no behaviour change, no lock, no wait. Pairs with
                 * stamp B just before smpuser_proof(). The surviving candidate is
                 * that fs_test_thread never reaches B inside the gate window
                 * because the ~30 user_boot_from_sfs() calls between A and B each
                 * block on SFS I/O over contended virtio-blk under -smp 4.
                 * B absent in a failing run confirms it; B early refutes it. */
                kputs("[boot-stamp] A probe-block-begin t="); kputdec(g_ticks); kputs("\r\n");
                /* DDR-812: commit the lockbox AFTER the boot self-tests and
                 * BEFORE the first user process exists, so no ring-3 code has
                 * ever run when the authoritative record is written. */
                metric_lockbox_commit_boot();
                kputs("[lockbox] committed at boot\r\n");
                user_boot_from_sfs(cap, smnt, "HELLO.ELF", hello_elf, hello_elf_end, 0);
                kputs("[wx] spawning W^X violator (expect a clean user-kill)\r\n");
                user_boot_from_sfs(cap, smnt, "WXVIOL.ELF", wx_elf, wx_elf_end, 0);
                /* Phase 5b: the syscall test program (read/write/open/... grows
                 * per slice). Runs in ring 3 and prints SYS* sentinels. */
                user_boot_from_sfs(cap, smnt, "SYSTEST.ELF", systest_elf, systest_elf_end, 0);
                /* L7 (DDR-703): ring-3 keyboard reader. Polls SYS_INPUT_POLL;
                 * the smoke-input gate injects keys via QEMU sendkey. */
                user_boot_from_sfs(cap, smnt, "INPUTTST.ELF", inputtest_elf, inputtest_elf_end, 0);
                /* L7 (DDR-704): the in-house compositor, spawned with CAP_SOVEREIGN
                 * so it may flip the mode via SYS_SET_MODE. With a GPU it renders
                 * the sovereign desktop and reacts to the keyboard; without one it
                 * exits via SYS_FB_INFO -> -ENODEV. Exercised by smoke-compositor.
                 * rq-1: spawned BEFORE the surface client — per-CPU runqueues let
                 * the client sprint through its whole setup+close before a later-
                 * spawned compositor even initializes, so the compositor never saw
                 * the 3-surface set and smoke-winops' SURFACE_GONE never fired. The
                 * desktop-first order is also the natural one. */
                user_boot_from_sfs(cap, smnt, "COMPOSIT.ELF",
                                   compositor_elf, compositor_elf_end,
                                   1 /* CAP_SOVEREIGN before first run */);
                /* L7 (DDR-706): a client window — creates + commits a surface that
                 * the compositor composites onto the desktop. Exercised by smoke-surface. */
                user_boot_from_sfs(cap, smnt, "SURFTEST.ELF", surfacetest_elf, surfacetest_elf_end, 0);
                /* L7 (DDR-729): surface lifecycle/destroy test — proves the table
                 * reclaims slots on close AND on process exit (a client that dies
                 * without SYS_SURFACE_CLOSE). Exercised by smoke-surfdestroy. */
                user_boot_from_sfs(cap, smnt, "SURFDEST.ELF", surfdestroytest_elf, surfdestroytest_elf_end, 0);
                /* L7 (DDR-730): per-agent live-metrics probe — polls SYS_AGENT_METRICS
                 * and asserts the daemon's KRYOS agent reports live. Exercised by
                 * smoke-agentmetrics. */
                user_boot_from_sfs(cap, smnt, "AGMETRIC.ELF", agentmetricstest_elf, agentmetricstest_elf_end, 0);
                /* L6/7 (DDR-731): CAP_NET probe — spawned CAP-less; asserts the
                 * socket NSI refuses it (-EPERM on connect, and on touching a
                 * slot it doesn't own). Exercised by smoke-capnet. */
                user_boot_from_sfs(cap, smnt, "CAPNET.ELF", capnettest_elf, capnettest_elf_end, 0);
                /* sys (DDR-748): SYS_SYSINFO introspection probe — default root,
                 * no special caps; prints CPU vendor/brand + PRADYOS_SYSINFO_OK.
                 * Gate smoke-sysinfo. */
                user_boot_from_sfs(cap, smnt, "SYSINFO.ELF", sysinfotest_elf, sysinfotest_elf_end, 0);
                /* F#68/DDR-795: reads the sealed objective root, then stores to
                 * it. The store must fault — idt.c kills the process, so
                 * METRIC_WX_FAIL is never printed. */
                user_boot_from_sfs(cap, smnt, "METRIC.ELF", metrictest_elf, metrictest_elf_end, 0);
                /* DDR-796 (BUG-1): asserts SYS_CLOCK never runs backwards. The
                 * CMOS index/data pair is chipset-global; unserialised, two CPUs
                 * read each other's register. */
                user_boot_from_sfs(cap, smnt, "RTCMONO.ELF", rtcmonotest_elf, rtcmonotest_elf_end, 0);
                /* DDR-800 (R1): spawned SOVEREIGN with is_net = 0 (the default
                 * from sched_create) — the exact shape the audit is about: no
                 * CAP_NET, destination off the allowlist, allowed anyway by the
                 * operator flag. It asserts the connect is still permitted AND
                 * that AR_SOVEREIGN_BYPASS recorded where it went. */
                user_boot_from_sfs(cap, smnt, "SOVEGRESS.ELF",
                                   sovegresstest_elf, sovegresstest_elf_end, 1);
                /* DDR-801 (R3): spawned with CAP_NET but NOT sovereign — so
                 * neither record it asserts can come from the DDR-800 bypass
                 * path. user_boot_from_sfs cannot grant is_net, so this uses the
                 * elf_load + set-flag-before-unblock pattern (cap-2a D3). */
                {
                    struct tcb *ea = 0;
                    uint64_t ealen = (uint64_t)((uintptr_t)egressaudittest_elf_end - (uintptr_t)egressaudittest_elf);
                    if (elf_load((void *)(uintptr_t)egressaudittest_elf, ealen,
                                 "EGRESSAUD", &ea) == ELF_OK && ea) {
                        ea->is_net = 1;          /* CAP_NET, no sovereign */
                        sched_unblock(ea);
                        kputs("[user] ELF loaded (embedded); egress-audit probe spawned\r\n");
                    }
                }
                /* DDR-802 gate, opt-in via DDR-804. This probe switches PRIVACY
                 * MODE — global kernel state — on and off. Spawning it in every
                 * gate would refuse the concurrent connects in capnettest /
                 * sovegresstest / egressaudittest and fail THEIR gates at
                 * random, so it runs only when smoke-privacy-netfilter selects
                 * it. Sovereign AND CAP_NET: the strongest credential the
                 * machine issues, so refusing it proves everyone is refused. */
                /* DDR-805 gate, opt-in via DDR-804. Self-contained: it writes
                 * to its own pipes and is expected to be KILLED by SIGPIPE, so
                 * it mutates no global state — but it is gated anyway, because
                 * a probe that dies by signal in every boot would add a corpse
                 * and its markers to all ~113 gates for no benefit. */
                /* DDR-811: SHA-256 vector probe, opt-in via DDR-804. */
                /* DDR-812: lockbox read/verify. Spawned SOVEREIGN because
                 * SYS_METRIC_READ is sovereign-gated — the record is the
                 * owner's ground truth, so a non-sovereign reader gets
                 * -EPERM and an audit entry. */
                /* DDR-996: force the freed-while-queued window deterministically.
                 * The CI artefact arose from a spawn/exit storm (init respawning
                 * a failing service), which is rare by nature. This drives the
                 * IDENTICAL path — sched_free_tcb reached with rq_on still set —
                 * without depending on a race: create a thread, make it runnable
                 * so it is pushed onto a runqueue, and destroy it before it is
                 * ever dispatched. That is a real scenario in its own right
                 * (spawn-failure cleanup), and it is the same defect: a TCB
                 * freed while a queue still points at it.
                 *
                 * IRQs off across the unblock/destroy pair so this CPU cannot
                 * reschedule into the victim. Another CPU may still steal and
                 * run it, which is why the gate asserts caught > 0 rather than
                 * caught == N — the point is that the state ARISES, and an
                 * exact count would be asserting the absence of work stealing. */
                if (probe_enabled("rqfree")) {
                    extern volatile uint32_t g_rqfree_caught, g_rqfree_leaked;
                    int made = sched_rqfree_probe(16);
                    kputs("[rqfree] made=");   kputdec((uint64_t)made);
                    kputs(" caught=");
                    kputdec((uint64_t)__atomic_load_n(&g_rqfree_caught, __ATOMIC_RELAXED));
                    kputs("\r\n");
                    /* Arm B: the queues must still be walkable afterwards. On an
                     * unfixed kernel this is where the poison surfaces. */
                    kputs("[rqfree] leaked=");
                    kputdec((uint64_t)__atomic_load_n(&g_rqfree_leaked, __ATOMIC_RELAXED));
                    kputs("\r\n");
                    if (__atomic_load_n(&g_rqfree_leaked, __ATOMIC_RELAXED) != 0 || !sched_rq_walk_ok())
                        kputs("RQFREE FAIL: a runqueue still points at a freed TCB\r\n");
                    else
                        kputs("PRADYOS_RQFREE_OK\r\n");
                }
                /* DDR-994: the yield-stall detector. Two arms, and arm A
                 * alone would be vacuous — it proves the reporter works, not
                 * that anything CALLS it. See vfs.c for why arm B uses a
                 * private scratch mount rather than a live one.
                 *
                 * Opt-in (DDR-804): arm B holds a waiter spinning for ~7 s by
                 * design, which the other 152 gates must not pay for. */
                if (probe_enabled("yieldstall")) {
                    /* Arm A — the reporter fires ONCE, not once per spin. Called
                     * three times with one `noted`; exactly one line must appear.
                     * A reporter that printed every iteration would bury the
                     * boot in UART traffic and move the timing it measures,
                     * which is why DDR-980 removed the cputicks heartbeat. */
                    int noted = 0;
                    yield_stall_note("selftest", 12345u, 678u, &noted);
                    yield_stall_note("selftest", 99999u, 999u, &noted);
                    yield_stall_note("selftest", 11111u, 111u, &noted);

                    /* Arm B — through the REAL mnt_lock. Arm the scratch mount
                     * as held, spawn a waiter, let it cross the threshold, then
                     * release it so the boot continues. */
                    vfs_yieldstall_arm(1);
                    struct tcb *ys = sched_create_blocked(yieldstall_waiter_thread, 0, "yieldstall");
                    if (ys) {
                        sched_unblock(ys);
                        /* YIELD_STALL_TICKS is 500 (5 s); wait past it with
                         * margin, then release. The waiter reports at the
                         * threshold and exits once the lock is free. */
                        uint64_t deadline = g_ticks + 700;
                        while (g_ticks < deadline)
                            yield();
                        vfs_yieldstall_arm(0);
                        kputs("PRADYOS_YIELDSTALL_RELEASED\r\n");
                    }
                }
                /* DDR-991: PS/2 modifier / extended-key probe. Opt-in — it
                 * announces PRADYOS_MODKEYS_WAIT and then spins polling for
                 * injected keys, so running it in every gate would add a
                 * spinning process to all 150 boots for no benefit. */
                if (probe_enabled("modkeys")) {
                    struct tcb *mk = 0;
                    /* DDR-993 §3: the paired-modifier arm runs HERE, in ring 0,
                     * and not as a sixth arm of the ring-3 probe. QEMU's HMP
                     * `sendkey` couples every press to its own release, so the
                     * one sequence that reaches the defect — two keys of a pair
                     * held at once, then ONE released — cannot be injected at
                     * all. Driving ps2kbd_feed directly is the only way to
                     * assert it, which is why DDR-993 §2 split the decode out
                     * of the port read. Runs before the probe is spawned and
                     * before any key is injected; it restores every byte of
                     * decoder state either way. */
                    int kst = ps2kbd_selftest();
                    if (kst) {
                        kputs("MODKEYS FAIL: kernel arm — ps2kbd_selftest step ");
                        kputdec((uint64_t)kst);
                        kputs(" (paired modifier / make-break identity)\r\n");
                    } else {
                        kputs("PRADYOS_MODKEYS_PAIR_OK\r\n");
                    }
                    uint64_t mklen = (uint64_t)((uintptr_t)modkeystest_elf_end - (uintptr_t)modkeystest_elf);
                    if (elf_load((void *)(uintptr_t)modkeystest_elf, mklen,
                                 "MODKEYS", &mk) == ELF_OK && mk) {
                        sched_unblock(mk);
                        kputs("[user] ELF loaded (embedded); modkeys probe spawned\r\n");
                    }
                }
                /* DDR-990: the two-CPU connect/close hammer. Opt-in via DDR-804
                 * and gated for a reason: it issues 20,000 connect/close pairs
                 * per instance, so spawning it in all 149 gates would add load
                 * and jitter to every one of them for no benefit — the same
                 * reasoning recorded above for the privacy-netfilter probe.
                 *
                 * TWO instances, because a single-CPU hammer proves NOTHING
                 * about a cross-CPU race. smp_resched_all() after spawning is
                 * the DDR-966 lesson: workers spawned without it sit on halted
                 * APs while the BSP burns the deadline, and both hammers would
                 * end up serialised on one CPU — a green run that measured the
                 * wrong thing entirely.
                 *
                 * is_net = 1 and NOT sovereign. Sovereign would bypass the
                 * allowlist for free, but it audits every connect
                 * (AR_SOVEREIGN_BYPASS, DDR-800); at 40,000 connects that churn
                 * would dominate the timing this probe exists to measure —
                 * DDR-947's lesson, where printing a thread name per heartbeat
                 * moved a failure rate from 2/12 to 9/14.
                 *
                 * The allowlist row is seeded HERE rather than assumed. Without
                 * it every connect returns an audited -EPERM, the probe hammers
                 * nothing, and it still prints its OK sentinel — a green result
                 * from a probe that never entered lwIP. The gate asserts
                 * conn_err=0, which is what turns that from a hope into a
                 * check (DDR-990 §5). */
                if (probe_enabled("nethammer")) {
                    int netallow_add(uint32_t host_be, uint16_t port);
                    (void)netallow_add(0x7F000001u, 8007);   /* 127.0.0.1:8007 */
                    uint64_t nhlen = (uint64_t)((uintptr_t)nethammer_elf_end - (uintptr_t)nethammer_elf);
                    int spawned = 0;
                    for (int nh_i = 0; nh_i < 2; nh_i++) {
                        struct tcb *nh = 0;
                        if (elf_load((void *)(uintptr_t)nethammer_elf, nhlen,
                                     "NETHAMMER", &nh) == ELF_OK && nh) {
                            nh->is_net = 1;      /* CAP_NET, deliberately not sovereign */
                            sched_unblock(nh);
                            spawned++;
                        }
                    }
                    smp_resched_all();           /* DDR-966: wake idle APs to take them */
                    kputs("[user] ELF loaded (embedded); net hammer spawned=");
                    kputdec((uint64_t)spawned);
                    kputs("/2\r\n");
                }
                /* DDR-818: HKDF RFC 5869 vector probe, opt-in via DDR-804. */
                if (probe_enabled("hkdf")) {
                    struct tcb *hk = 0;
                    uint64_t hklen = (uint64_t)((uintptr_t)hkdftest_elf_end - (uintptr_t)hkdftest_elf);
                    if (elf_load((void *)(uintptr_t)hkdftest_elf, hklen,
                                 "HKDFTEST", &hk) == ELF_OK && hk) {
                        sched_unblock(hk);
                        kputs("[user] ELF loaded (embedded); HKDF vector probe spawned\r\n");
                    }
                }
                /* DDR-820: X25519 RFC 7748 vector probe, opt-in via DDR-804. */
                if (probe_enabled("x25519")) {
                    struct tcb *xk = 0;
                    uint64_t xklen = (uint64_t)((uintptr_t)x25519test_elf_end - (uintptr_t)x25519test_elf);
                    if (elf_load((void *)(uintptr_t)x25519test_elf, xklen,
                                 "X25519", &xk) == ELF_OK && xk) {
                        sched_unblock(xk);
                        kputs("[user] ELF loaded (embedded); X25519 vector probe spawned\r\n");
                    }
                }
                /* DDR-821: SHA-512 FIPS 180-4 vector probe, opt-in via DDR-804. */
                if (probe_enabled("sha512")) {
                    struct tcb *s5 = 0;
                    uint64_t s5len = (uint64_t)((uintptr_t)sha512test_elf_end - (uintptr_t)sha512test_elf);
                    if (elf_load((void *)(uintptr_t)sha512test_elf, s5len,
                                 "SHA512", &s5) == ELF_OK && s5) {
                        sched_unblock(s5);
                        kputs("[user] ELF loaded (embedded); SHA-512 vector probe spawned\r\n");
                    }
                }
                /* DDR-819: ChaCha20-Poly1305 vector probe, opt-in via DDR-804.
                 * Closes the gap where aead.c was host-verified only, with no
                 * gate and no build wiring — DDR-813 must not consume it
                 * ungated. */
                if (probe_enabled("aead")) {
                    struct tcb *ae = 0;
                    uint64_t aelen = (uint64_t)((uintptr_t)aeadtest_elf_end - (uintptr_t)aeadtest_elf);
                    if (elf_load((void *)(uintptr_t)aeadtest_elf, aelen,
                                 "AEAD", &ae) == ELF_OK && ae) {
                        sched_unblock(ae);
                        kputs("[user] ELF loaded (embedded); AEAD vector probe spawned\r\n");
                    }
                }
                /* DDR-821: Ed25519 RFC 8032 vector probe, opt-in via DDR-804. */
                if (probe_enabled("ed25519")) {
                    struct tcb *ed = 0;
                    uint64_t edlen = (uint64_t)((uintptr_t)ed25519test_elf_end - (uintptr_t)ed25519test_elf);
                    if (elf_load((void *)(uintptr_t)ed25519test_elf, edlen,
                                 "ED25519", &ed) == ELF_OK && ed) {
                        sched_unblock(ed);
                        kputs("[user] ELF loaded (embedded); Ed25519 vector probe spawned\r\n");
                    }
                }
                /* DDR-813: ACC envelope gate, opt-in via DDR-804. */
                if (probe_enabled("acc")) {
                    struct tcb *ac = 0;
                    uint64_t aclen = (uint64_t)((uintptr_t)acctest_elf_end - (uintptr_t)acctest_elf);
                    if (elf_load((void *)(uintptr_t)acctest_elf, aclen,
                                 "ACCTEST", &ac) == ELF_OK && ac) {
                        sched_unblock(ac);
                        kputs("[user] ELF loaded (embedded); ACC gate probe spawned\r\n");
                    }
                }
                if (probe_enabled("lockbox")) {
                    struct tcb *lb = 0;
                    uint64_t lblen = (uint64_t)((uintptr_t)lockboxtest_elf_end - (uintptr_t)lockboxtest_elf);
                    if (elf_load((void *)(uintptr_t)lockboxtest_elf, lblen,
                                 "LOCKBOX", &lb) == ELF_OK && lb) {
                        lb->is_sovereign = 1;
                        sched_unblock(lb);
                        kputs("[user] ELF loaded (embedded); lockbox probe spawned\r\n");
                    }
                }
                /* DDR-844: S1-S8 attack gate. Spawned as an AGENT with
                 * CAP_MEMORY and nothing else, DELIBERATELY — a sovereign probe
                 * would be permitted to do most of what this attempts and the
                 * gate would assert nothing. */
                if (probe_enabled("invariants")) {
                    struct tcb *iv = 0;
                    uint64_t l = (uint64_t)((uintptr_t)invarianttest_elf_end - (uintptr_t)invarianttest_elf);
                    int rc = elf_load((void *)(uintptr_t)invarianttest_elf, l, "INVARIANT", &iv);
                    if (rc == ELF_OK && iv) {
                        iv->is_agent = 1;
                        iv->is_memory = 1;   /* so the S2/S6 memory arms reach the bounds checks */
                        sched_unblock(iv);
                        kputs("[user] invariant attack probe spawned (agent)\r\n");
                    } else {
                        kputs("[user] INVARIANT elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)rc);
                        kputs("\r\n");
                    }
                }
                /* DDR-842 item 6: FOUR roles. The capability split IS the
                 * feature, and no single privilege level can test it. */
                if (probe_enabled("coderewrite")) {
                    uint64_t l = (uint64_t)((uintptr_t)coderewritetest_elf_end - (uintptr_t)coderewritetest_elf);
                    struct tcb *ag = 0, *ap = 0, *so = 0, *rw = 0;
                    if (elf_load((void *)(uintptr_t)coderewritetest_elf, l, "CRW_AG", &ag) == ELF_OK && ag) {
                        ag->is_agent = 1; ag->is_memory = 1; sched_unblock(ag);
                    } else { kputs("[user] CRW agent elf_load FAILED\r\n"); }
                    if (elf_load((void *)(uintptr_t)coderewritetest_elf, l, "CRW_AP", &ap) == ELF_OK && ap) {
                        ap->is_sovereign = 1; ap->is_rewrite = 1; ap->is_memory = 1; sched_unblock(ap);
                    } else { kputs("[user] CRW approver elf_load FAILED\r\n"); }
                    if (elf_load((void *)(uintptr_t)coderewritetest_elf, l, "CRW_SO", &so) == ELF_OK && so) {
                        so->is_sovereign = 1; so->is_memory = 1;   /* NO is_rewrite */
                        sched_unblock(so);
                    } else { kputs("[user] CRW sovonly elf_load FAILED\r\n"); }
                    if (elf_load((void *)(uintptr_t)coderewritetest_elf, l, "CRW_RW", &rw) == ELF_OK && rw) {
                        rw->is_rewrite = 1;                        /* NO is_sovereign */
                        sched_unblock(rw);
                    } else { kputs("[user] CRW rwonly elf_load FAILED\r\n"); }
                    kputs("[user] code-rewrite probes spawned (4 roles)\r\n");
                }
                /* DDR-842 item 7. audittamper is a SEPARATE probe flag so one
                 * binary serves both the intact and the tampered gate. */
                if (probe_enabled("auditchain")) {
                    if (probe_enabled("audittamper"))
                        aether_audit_tamper();
                    struct tcb *ac = 0;
                    uint64_t l = (uint64_t)((uintptr_t)auditchaintest_elf_end - (uintptr_t)auditchaintest_elf);
                    int rc = elf_load((void *)(uintptr_t)auditchaintest_elf, l, "AUDITCHAIN", &ac);
                    if (rc == ELF_OK && ac) {
                        ac->is_sovereign = 1;
                        sched_unblock(ac);
                        kputs("[user] audit-chain probe spawned\r\n");
                    } else {
                        kputs("[user] AUDITCHAIN elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)rc);
                        kputs("\r\n");
                    }
                }
                /* DDR-839: DAG action queue. TWO roles — submission is CAP_AGENT
                 * and approval is CAP_SOVEREIGN, so no single privilege level can
                 * drive this gate alone. Both hold CAP_MEMORY to rendezvous on
                 * the action ids. */
                if (probe_enabled("actiondag")) {
                    uint64_t dlen = (uint64_t)((uintptr_t)actiondagtest_elf_end - (uintptr_t)actiondagtest_elf);
                    struct tcb *d_ag = 0, *d_sov = 0;
                    if (elf_load((void *)(uintptr_t)actiondagtest_elf, dlen,
                                 "DAG_A", &d_ag) == ELF_OK && d_ag) {
                        d_ag->is_agent = 1;
                        d_ag->is_memory = 1;
                        sched_unblock(d_ag);
                        kputs("[user] action-DAG agent spawned\r\n");
                    } else {
                        kputs("[user] ACTIONDAG agent elf_load FAILED\r\n");
                    }
                    if (elf_load((void *)(uintptr_t)actiondagtest_elf, dlen,
                                 "DAG_S", &d_sov) == ELF_OK && d_sov) {
                        d_sov->is_sovereign = 1;
                        d_sov->is_memory = 1;
                        sched_unblock(d_sov);
                        kputs("[user] action-DAG sovereign spawned\r\n");
                    } else {
                        kputs("[user] ACTIONDAG sovereign elf_load FAILED\r\n");
                    }
                }
                /* DDR-838: spawn-depth cap. Spawned as an AGENT, because the cap
                 * only applies to agent lineages — a non-agent founder would sit
                 * at depth 0 forever and the gate would prove nothing. */
                if (probe_enabled("spawndepth")) {
                    struct tcb *sd = 0;
                    uint64_t sdlen = (uint64_t)((uintptr_t)spawndepthtest_elf_end - (uintptr_t)spawndepthtest_elf);
                    int sdrc = elf_load((void *)(uintptr_t)spawndepthtest_elf, sdlen,
                                        "SPAWNDEPTH", &sd);
                    if (sdrc == ELF_OK && sd) {
                        sd->is_agent = 1;
                        sched_unblock(sd);
                        kputs("[user] spawn-depth probe spawned (agent)\r\n");
                    } else {
                        kputs("[user] SPAWNDEPTH elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)sdrc);
                        kputs("\r\n");
                    }
                }
                /* DDR-837: checkpoint/resume gate. THREE instances — a sovereign
                 * controller, a plain target named CKPT_T that the controller
                 * finds by name, and an agent that must be denied. The target is
                 * deliberately NOT an agent: agents are capped at 60 syscalls/s
                 * and its yield loop would be killed at 137 mid-gate. */
                if (probe_enabled("ckpt")) {
                    uint64_t clen = (uint64_t)((uintptr_t)ckpttest_elf_end - (uintptr_t)ckpttest_elf);
                    struct tcb *c_ctl = 0, *c_tgt = 0, *c_deny = 0;
                    if (elf_load((void *)(uintptr_t)ckpttest_elf, clen,
                                 "CKPT_T", &c_tgt) == ELF_OK && c_tgt) {
                        sched_unblock(c_tgt);
                        kputs("[user] checkpoint target spawned\r\n");
                    } else {
                        kputs("[user] CKPT target elf_load FAILED\r\n");
                    }
                    if (elf_load((void *)(uintptr_t)ckpttest_elf, clen,
                                 "CKPT_C", &c_ctl) == ELF_OK && c_ctl) {
                        c_ctl->is_sovereign = 1;
                        sched_unblock(c_ctl);
                        kputs("[user] checkpoint controller spawned\r\n");
                    } else {
                        kputs("[user] CKPT controller elf_load FAILED\r\n");
                    }
                    if (elf_load((void *)(uintptr_t)ckpttest_elf, clen,
                                 "CKPT_D", &c_deny) == ELF_OK && c_deny) {
                        c_deny->is_agent = 1;
                        sched_unblock(c_deny);
                        kputs("[user] checkpoint deny probe spawned\r\n");
                    } else {
                        kputs("[user] CKPT deny elf_load FAILED\r\n");
                    }
                }
                /* DDR-836: agent memory gate. Spawned TWICE — one instance holds
                 * CAP_MEMORY, the other does not — because the capability check
                 * is half of what is under test. */
                if (probe_enabled("agentmem")) {
                    uint64_t mlen = (uint64_t)((uintptr_t)agentmemtest_elf_end - (uintptr_t)agentmemtest_elf);
                    struct tcb *m_cap = 0, *m_no = 0;
                    int mrc_c = elf_load((void *)(uintptr_t)agentmemtest_elf, mlen,
                                         "AGENTMEM_C", &m_cap);
                    if (mrc_c == ELF_OK && m_cap) {
                        m_cap->is_agent = 1;
                        m_cap->is_memory = 1;
                        sched_unblock(m_cap);
                        kputs("[user] agent-memory probe spawned (CAP_MEMORY)\r\n");
                    } else {
                        kputs("[user] AGENTMEM cap elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)mrc_c);
                        kputs("\r\n");
                    }
                    int mrc_n = elf_load((void *)(uintptr_t)agentmemtest_elf, mlen,
                                         "AGENTMEM_N", &m_no);
                    if (mrc_n == ELF_OK && m_no) {
                        m_no->is_agent = 1;      /* an agent, but WITHOUT CAP_MEMORY */
                        sched_unblock(m_no);
                        kputs("[user] agent-memory probe spawned (no CAP_MEMORY)\r\n");
                    } else {
                        kputs("[user] AGENTMEM nocap elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)mrc_n);
                        kputs("\r\n");
                    }
                }
                /* DDR-834: credential vault gate. Spawned TWICE at different
                 * privilege for the same reason as the rotation probe: the
                 * capability split is part of what is under test. */
                if (probe_enabled("vault")) {
                    uint64_t vlen = (uint64_t)((uintptr_t)vaulttest_elf_end - (uintptr_t)vaulttest_elf);
                    struct tcb *v_sov = 0, *v_ag = 0;
                    int vrc_s = elf_load((void *)(uintptr_t)vaulttest_elf, vlen,
                                         "VAULT_S", &v_sov);
                    if (vrc_s == ELF_OK && v_sov) {
                        v_sov->is_sovereign = 1;
                        sched_unblock(v_sov);
                        kputs("[user] vault probe spawned (sovereign)\r\n");
                    } else {
                        kputs("[user] VAULT sovereign elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)vrc_s);
                        kputs("\r\n");
                    }
                    int vrc_a = elf_load((void *)(uintptr_t)vaulttest_elf, vlen,
                                         "VAULT_A", &v_ag);
                    if (vrc_a == ELF_OK && v_ag) {
                        v_ag->is_agent = 1;
                        sched_unblock(v_ag);
                        kputs("[user] vault probe spawned (agent)\r\n");
                    } else {
                        kputs("[user] VAULT agent elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)vrc_a);
                        kputs("\r\n");
                    }
                }
                /* DDR-815: ACC rotation gate. The SAME ELF is spawned TWICE —
                 * once sovereign, once agent-only — because the capability split
                 * is the thing under test and one privilege level can only ever
                 * test half of it. The probe tells the instances apart from
                 * rotate's return code (-ENOENT vs -EPERM), so no argument
                 * channel is needed. */
                if (probe_enabled("accrot")) {
                    uint64_t arlen = (uint64_t)((uintptr_t)accrottest_elf_end - (uintptr_t)accrottest_elf);
                    struct tcb *ar_sov = 0, *ar_ag = 0;
                    int rc_sov = elf_load((void *)(uintptr_t)accrottest_elf, arlen,
                                          "ACCROT_S", &ar_sov);
                    if (rc_sov == ELF_OK && ar_sov) {
                        ar_sov->is_sovereign = 1;
                        sched_unblock(ar_sov);
                        kputs("[user] ACC rotation probe spawned (sovereign)\r\n");
                    } else {
                        kputs("[user] ACCROT sovereign elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)rc_sov);
                        kputs("\r\n");
                    }
                    int rc_ag = elf_load((void *)(uintptr_t)accrottest_elf, arlen,
                                         "ACCROT_A", &ar_ag);
                    if (rc_ag == ELF_OK && ar_ag) {
                        ar_ag->is_agent = 1;
                        sched_unblock(ar_ag);
                        kputs("[user] ACC rotation probe spawned (agent)\r\n");
                    } else {
                        kputs("[user] ACCROT agent elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)rc_ag);
                        kputs("\r\n");
                    }
                }
                /* DDR-814: AGS goal-signing gate, opt-in via DDR-804. Sovereign
                 * because arm 1 calls SYS_GOAL_SIGN, which is CAP_SOVEREIGN. */
                if (probe_enabled("ags")) {
                    struct tcb *ag = 0;
                    uint64_t aglen = (uint64_t)((uintptr_t)agstest_elf_end - (uintptr_t)agstest_elf);
                    int agrc = elf_load((void *)(uintptr_t)agstest_elf, aglen,
                                        "AGS", &ag);
                    if (agrc == ELF_OK && ag) {
                        ag->is_sovereign = 1;
                        sched_unblock(ag);
                        kputs("[user] ELF loaded (embedded); AGS goal probe spawned\r\n");
                    } else {
                        /* OPEN-11's lesson: a silent load failure is
                         * indistinguishable from a probe that ran and said
                         * nothing, and cost five sessions. */
                        kputs("[user] AGS probe elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)agrc);
                        kputs("\r\n");
                    }
                }
                if (probe_enabled("sha256")) {
                    struct tcb *sh = 0;
                    uint64_t shlen = (uint64_t)((uintptr_t)sha256test_elf_end - (uintptr_t)sha256test_elf);
                    /* OPEN-11: this spawn used to be `if (... == ELF_OK && sh)`
                     * with no else, so a failed load printed NOTHING and the gate
                     * timed out looking like the probe had run and stayed silent.
                     * A load that fails must say so, and say why. */
                    int shrc = elf_load((void *)(uintptr_t)sha256test_elf, shlen,
                                        "SHA256", &sh);
                    if (shrc == ELF_OK && sh) {
                        sched_unblock(sh);
                        kputs("[user] ELF loaded (embedded); SHA-256 vector probe spawned\r\n");
                    } else {
                        kputs("[user] SHA-256 probe elf_load FAILED rc=");
                        kputdec((uint64_t)(int64_t)shrc);
                        kputs(" free_frames=");
                        kputhex(pmm_free_page_count());
                        kputs("\r\n");
                    }
                }
                if (probe_enabled("sigpipe")) {
                    struct tcb *sp = 0;
                    uint64_t splen = (uint64_t)((uintptr_t)sigpipetest_elf_end - (uintptr_t)sigpipetest_elf);
                    if (elf_load((void *)(uintptr_t)sigpipetest_elf, splen,
                                 "SIGPIPE", &sp) == ELF_OK && sp) {
                        sched_unblock(sp);
                        kputs("[user] ELF loaded (embedded); SIGPIPE probe spawned\r\n");
                    }
                }
                if (probe_enabled("privnet")) {
                    struct tcb *pn = 0;
                    uint64_t pnlen = (uint64_t)((uintptr_t)privacynettest_elf_end - (uintptr_t)privacynettest_elf);
                    if (elf_load((void *)(uintptr_t)privacynettest_elf, pnlen,
                                 "PRIVNET", &pn) == ELF_OK && pn) {
                        pn->is_net = 1;
                        pn->is_sovereign = 1;
                        sched_unblock(pn);
                        kputs("[user] ELF loaded (embedded); privacy-netfilter probe spawned\r\n");
                    }
                }
                /* sys (DDR-749): SYS_TIME wall-clock probe — default root, no caps;
                 * prints TIME YYYY-MM-DD HH:MM:SS + PRADYOS_TIME_OK. Gate smoke-time. */
                user_boot_from_sfs(cap, smnt, "TIME.ELF", timetest_elf, timetest_elf_end, 0);
                /* sys (DDR-750): SYS_DMESG probe — writes a marker then reads the
                 * kernel log back and confirms it. Gate smoke-dmesg. */
                user_boot_from_sfs(cap, smnt, "DMESG.ELF", dmesgtest_elf, dmesgtest_elf_end, 0);
                /* proc (DDR-755): fork/kill/reap probe — forks a child that spins
                 * forever, SIGKILLs it, reaps it -> PRADYOS_KILL_OK. Gate smoke-kill. */
                user_boot_from_sfs(cap, smnt, "KILL.ELF", killtest_elf, killtest_elf_end, 0);
                /* proc (DDR-756): self-rename probe — SYS_SETNAME then verifies the
                 * new name via SYS_GETPROCS -> PRADYOS_SETNAME_OK. Gate smoke-setname. */
                user_boot_from_sfs(cap, smnt, "SETNAME.ELF", setnametest_elf, setnametest_elf_end, 0);
                /* sec (DDR-758): hostile-syscall fuzz — 3000 bad NSI numbers +
                 * wild pointers; kernel must survive -> PRADYOS_FUZZ_OK. Gate
                 * smoke-syscallfuzz. */
                user_boot_from_sfs(cap, smnt, "FUZZ.ELF", syscallfuzz_elf, syscallfuzz_elf_end, 0);
                /* fs (DDR-739): per-process root-mount probe. Spawned from its
                 * embedded bytes (not via SFS — root_mnt is set BEFORE unblock,
                 * which user_boot_from_sfs doesn't allow) with root_mnt pointed at
                 * the ext4 mount, only when blk3 is present. It proves SYS_OPEN
                 * resolves against the selected root. Exercised by smoke-rootmount. */
                if (ext4_mnt >= 0) {
                    struct tcb *rmp = 0;
                    uint64_t rlen = (uint64_t)((uintptr_t)rootmounttest_elf_end - (uintptr_t)rootmounttest_elf);
                    if (elf_load((void *)(uintptr_t)rootmounttest_elf, rlen,
                                 "ROOTMNT", &rmp) == ELF_OK && rmp) {
                        rmp->root_mnt = ext4_mnt;     /* select before unblock (cap-2a D3) */
                        sched_unblock(rmp);
                        kputs("[user] ELF loaded (embedded); ext4-rooted probe spawned\r\n");
                    }
                }
                /* fs (DDR-744): ring-3 file-lifecycle probe (O_CREAT open +
                 * SYS_UNLINK). Spawned with root_mnt = the SFS volume (the
                 * writable CoW root; FAT has no ring-3 create/unlink here), set
                 * before unblock like the ext4 probe. Proves the two new VFS
                 * mutations across the syscall boundary. Gate: smoke-fsrm. */
                /* DDR-880: stamp C. Every captured failure so far stops between
                 * stamp A and stamp B with the ext4 probe as the last thing
                 * spawned — a ~90-line window. This narrows it to one statement:
                 * if C prints and B does not, the thread is lost AFTER the ext4
                 * block and the next elf_load is the suspect; if C never prints,
                 * it is lost inside the ext4 block itself. */
                kputs("[boot-stamp] C ext4-done t="); kputdec(g_ticks); kputs("\r\n");
                {
                    struct tcb *fp = 0;
                    uint64_t flen = (uint64_t)((uintptr_t)fsrmtest_elf_end - (uintptr_t)fsrmtest_elf);
                    if (elf_load((void *)(uintptr_t)fsrmtest_elf, flen,
                                 "FSRMTEST", &fp) == ELF_OK && fp) {
                        fp->root_mnt = smnt;          /* SFS root before unblock  */
                        smnt_pid[0] = fp->pid;        /* DDR-967: wait on it below */
                        sched_unblock(fp);
                        kputs("[user] ELF loaded (embedded); SFS-rooted fsrm probe spawned\r\n");
                    }
                }
                /* DDR-866 (item 20): ring-3 ftruncate probe — shrink, grow
                 * with zero-fill, truncate-to-zero, negative-length refusal.
                 *
                 * OPT-IN via DDR-804, unlike the fsrm probe beside it, because
                 * this one CREATES A FILE ON THE SHARED SFS ROOT. Running it
                 * every boot would change what every other SFS gate sees, so a
                 * failure elsewhere could be this probe's leftovers. */
                /* ADR-038: demand-paged-stack probe. Behind probe_enabled
                 * because arm 3 deliberately overflows the guard page and dies —
                 * an unconditional spawn would put a killed process in every
                 * boot and every other gate's log. */
                if (probe_enabled("stackdemand")) {
                    struct tcb *sp2 = 0;
                    uint64_t slen = (uint64_t)((uintptr_t)stackdemand_elf_end - (uintptr_t)stackdemand_elf);
                    if (elf_load((void *)(uintptr_t)stackdemand_elf, slen,
                                 "STACKDEMAND", &sp2) == ELF_OK && sp2) {
                        sched_unblock(sp2);
                        kputs("[user] ELF loaded (embedded); stackdemand probe spawned\r\n");
                    } else {
                        kputs("[user] stackdemand probe FAILED to load\r\n");
                    }
                }
                /* DDR-973: FAT32 multi-cluster read regression probe.
                 *
                 * Rooted at `mnt` -- the FAT32 volume -- not `smnt`, because the
                 * FAT reader is what is under test. Every other probe in this
                 * block roots at SFS, so this one states it: `mnt` is the stable
                 * FAT mount chosen at the top of this function and made the
                 * process root by vfs_set_default_mnt (main.c:1128), and it is
                 * NOT reformatted by the destructive self-tests below.
                 *
                 * Opt-in (DDR-804) for the reason the ftruncate probe beside it
                 * is: its arm C execve's /CMUSL.ELF, whose PRADYOS_MUSL_OK would
                 * otherwise appear a second time in every other gate's log. The
                 * gate asserts that marker's COUNT, so the extra copy is the
                 * signal -- it must not be background noise.
                 *
                 * root_mnt is set BEFORE sched_unblock (DDR-957 ordering): this
                 * uses the raw elf_load path, so the assignment and the unblock
                 * are separate statements and the order is the whole point. */
                if (probe_enabled("fat32mc")) {
                    struct tcb *fm = 0;
                    /* DDR-970: cast to uintptr_t first -- two distinct linker
                     * symbols, so subtracting them as pointers is undefined
                     * (C11 6.5.6). */
                    uint64_t fmlen = (uint64_t)(uintptr_t)fat32mctest_elf_end -
                                     (uint64_t)(uintptr_t)fat32mctest_elf;
                    if (elf_load((void *)(uintptr_t)fat32mctest_elf, fmlen,
                                 "FAT32MC", &fm) == ELF_OK && fm) {
                        fm->root_mnt = mnt;           /* FAT root before unblock */
                        sched_unblock(fm);
                        kputs("[user] ELF loaded (embedded); FAT-rooted multi-cluster probe spawned\r\n");
                    } else {
                        kputs("[user] FAT32 multi-cluster probe FAILED to load\r\n");
                    }
                }
                /* DDR-1015: Section 3C ACTION_READ_FILE, end to end. Rooted at
                 * the FAT mount so the approved read has a real file to open,
                 * and root_mnt is set BEFORE sched_unblock for the DDR-957
                 * ordering reason spelled out above.
                 *
                 * is_agent, deliberately: an agent is the actor that holds no
                 * authority of its own, which is the property the pipeline
                 * exists to enforce. Sovereign mode is the ADR-026 D2 default
                 * (aether_queue.c:37), and ACTION_READ_FILE is not in
                 * aether_action_forces_pending(), so it auto-approves and this
                 * probe needs no second privileged actor -- unlike the four
                 * force-pending types, whose gates must assert PENDING instead
                 * (DDR-1013 §2.1). */
                if (probe_enabled("actionread")) {
                    struct tcb *ar = 0;
                    uint64_t arlen = (uint64_t)(uintptr_t)actionreadtest_elf_end -
                                     (uint64_t)(uintptr_t)actionreadtest_elf;
                    if (elf_load((void *)(uintptr_t)actionreadtest_elf, arlen,
                                 "ACTIONREAD", &ar) == ELF_OK && ar) {
                        ar->is_agent = 1;
                        ar->root_mnt = mnt;           /* FAT root before unblock */
                        sched_unblock(ar);
                        kputs("[user] 3C ACTION_READ_FILE probe spawned (agent, FAT-rooted)\r\n");
                    } else {
                        kputs("[user] ACTIONREAD probe FAILED to load\r\n");
                    }
                }
                if (probe_enabled("ftruncate")) {
                    struct tcb *tp = 0;
                    uint64_t tlen = (uint64_t)((uintptr_t)ftrunctest_elf_end - (uintptr_t)ftrunctest_elf);
                    if (elf_load((void *)(uintptr_t)ftrunctest_elf, tlen,
                                 "FTRUNCTEST", &tp) == ELF_OK && tp) {
                        tp->root_mnt = smnt;          /* SFS root before unblock */
                        smnt_pid[1] = tp->pid;        /* DDR-967 */
                        sched_unblock(tp);
                        kputs("[user] ELF loaded (embedded); ftruncate probe spawned\r\n");
                    }
                }
                /* DDR-962: SYS_RENAME on the SFS root. sfs_rename shipped in
                 * DDR-956 and has never been gated -- PRISM is FAT-rooted, so
                 * DDR-958's shell-driven smoke-rename proves fat32_rename only.
                 * Rooted at smnt for the same reason the ftruncate probe above
                 * is, and opt-in (DDR-804) for the same reason too: it creates
                 * files on the shared SFS root, so running it every boot would
                 * leave its droppings in every other gate's log. */
                if (probe_enabled("rename-sfs")) {
                    struct tcb *rn = 0;
                    /* DDR-970: cast to uintptr_t first. These are two distinct
                     * linker symbols, so subtracting them as pointers is
                     * undefined (C11 6.5.6). 29 pre-existing sites in this file
                     * share the idiom and are left alone -- see DDR-970 §4 --
                     * but this PR should not add a 30th. */
                    uint64_t rnlen = (uint64_t)(uintptr_t)renametest_elf_end -
                                     (uint64_t)(uintptr_t)renametest_elf;
                    if (elf_load((void *)(uintptr_t)renametest_elf, rnlen,
                                 "RENAMETEST", &rn) == ELF_OK && rn) {
                        rn->root_mnt = smnt;          /* SFS root before unblock */
                        smnt_pid[2] = rn->pid;        /* DDR-967 */
                        sched_unblock(rn);
                        kputs("[user] ELF loaded (embedded); SFS rename probe spawned\r\n");
                    } else {
                        kputs("[user] SFS rename probe FAILED to load\r\n");
                    }
                }
                /* DDR-870 (items 44/45): RDTSC benchmark of the syscall and
                 * context-switch paths. Opt-in via DDR-804: it runs thousands
                 * of syscalls, which would perturb the timing-sensitive gates
                 * if it ran on every boot. */
                if (probe_enabled("bench")) {
                    struct tcb *bp = 0;
                    uint64_t blen2 = (uint64_t)((uintptr_t)benchtest_elf_end - (uintptr_t)benchtest_elf);
                    if (elf_load((void *)(uintptr_t)benchtest_elf, blen2,
                                 "BENCHTEST", &bp) == ELF_OK && bp) {
                        sched_unblock(bp);
                        kputs("[user] ELF loaded (embedded); perf benchmark spawned\r\n");
                    }
                }
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
                /* DDR-957: PRISM stays on the FAT default root. Rooting it at
                 * `smnt` was tried and reverted -- that volume is REFORMATTED at
                 * sfs_format(sbd) later in boot, and smoke-shell asserts on
                 * FAT-only fixtures (/HELLO.TXT, /BIG8K.TXT, /EXECTEST.ELF).
                 * See DDR-957 sec.9-10 before trying again. */
                struct tcb *pr = user_boot_from_sfs(cap, smnt, "PRISM.ELF",
                                                    prism_elf, prism_elf_end, 0);
                if (pr && it)
                    pr->parent_pid = it->pid;

                /* L6: AETHER daemon as init's child, granted CAP_SOVEREIGN (it
                 * owns mode + approve authority). DDR-761: loaded BLOCKED here
                 * (hand-rolled elf_load, DDR-739 pattern) and rooted at the clean
                 * PERSISTENT SFS root + unblocked only AFTER the reformat+provision
                 * below, so it reads /etc/aether/config from the SFS volume. */
                struct tcb *dm = 0;
                {
                    uint64_t dlen = (uint64_t)((uintptr_t)aether_daemon_elf_end - (uintptr_t)aether_daemon_elf);
                    if (elf_load((void *)(uintptr_t)aether_daemon_elf, dlen,
                                 "AETHERD", &dm) == ELF_OK && dm) {
                        dm->is_sovereign = 1;         /* authority before first run */
                        if (it) dm->parent_pid = it->pid;
                        g_aether_daemon_pid = dm->pid;
                    }
                }
                /* DDR-806 settling measurement (stamp B). Its ABSENCE in a
                 * failing run is the confirmation: it means fs_test_thread never
                 * got here, so the proofs below could not have run — which is
                 * exactly what the missing OK/FAIL sentinels showed. */
                kputs("[boot-stamp] B proofs-begin t="); kputdec(g_ticks); kputs("\r\n");
                smpuser_proof();     /* ADR-031 cap-4: ring 3 runs on an AP */
                blkmq_proof();       /* DDR-BLK-1: concurrent in-flight requests */
                smp_blk_integrity(); /* DDR-759: concurrent-read DATA integrity (M1 audit) */
                rqstress_proof();    /* DDR-SMP-rq-1: thread storm over the rqs */
                /* DDR-895 (item 16 diagnostic): probe-gated, so no default boot
                 * changes. The trace is observation only — the pick is still
                 * FIFO in this build. */
                if (probe_enabled("schedtrace"))
                    sched_trace_dump();
                {
                    /* DDR-885 (item 37). Printed HERE because rqstress is the
                     * only workload that reliably produces steals: 24 threads in
                     * 3 waves across the per-CPU runqueues. Printed after it so
                     * the counts are non-zero and the gate can assert on them. */
                    uint64_t sl = 0, sr = 0;
                    sched_steal_counts(&sl, &sr);
                    kputs("[sched] steal local=");
                    kputdec(sl);
                    kputs(" remote=");
                    kputdec(sr);
                    kputs("\r\n");
                }
                /* DDR-714C3: assert a blk completion ran off the BSP. The old
                 * check-once was racy on slow TCG (the AP-routed completion hadn't
                 * necessarily landed by this boot point — recurring `-smp 4` flake,
                 * SESSION_HANDOFF audit finding). Root fix: poll with a bounded
                 * deadline while ISSUING blk0 reads — unit 0's MSI-X vector targets
                 * an AP, so each completion sets compl_ap. Deterministic: the loop
                 * forces the very event it waits for. */
                if (g_smp_have_aps) {
                    struct blk_device *b0 = blk_get(0);
                    uint64_t sbuf = pmm_alloc_page();
                    uint64_t dl = g_ticks + 200;             /* <= 2 s backstop */
                    while (!virtio_blk_completed_on_ap() && g_ticks < dl) {
                        if (b0 && sbuf)
                            b0->read(b0, 0, (void *)(uintptr_t)sbuf, 1);
                        else
                            yield();
                    }
                    if (sbuf)
                        pmm_free_page(sbuf);
                    kputs(virtio_blk_completed_on_ap()
                              ? "[blk] msix on AP OK\r\n"
                              : "[blk] msix on AP FAIL\r\n");
                }

                /* DDR-967: the block below REFORMATS the volume, and the three
                 * ring-3 probes above are rooted on it (->root_mnt = smnt) and
                 * runnable from their sched_unblock. Until now nothing waited
                 * for them, so whether a probe finished before this umount was
                 * pure scheduling luck — that is the FSRM "created file did not
                 * persist" race: create and write succeed, then the reopen finds
                 * a freshly formatted disk.
                 *
                 * Waited BY PID, not by TCB pointer. Polling ->state for
                 * THREAD_ZOMBIE is a use-after-free: sched_start_reaper() can
                 * free a zombie's TCB mid-poll. sched_find_pid() is documented
                 * "live thread by pid, or NULL", so it goes NULL once the thread
                 * is gone. (The main.c:611 crosswake precedent polls a TCB
                 * safely only because that thread blocks and never exits.)
                 *
                 * Bounded, and falls THROUGH on expiry to exactly today's
                 * behaviour: an intermittently-failing gate must not become a
                 * gate that wedges. A pid of 0 is a probe that was never
                 * spawned (probe_enabled) and is skipped. */
                {
                    uint64_t dl = g_ticks + 400;
                    for (int i = 0; i < 3; i++)
                        while (smnt_pid[i] && sched_find_pid(smnt_pid[i]) && g_ticks < dl)
                            yield();
                    if (g_ticks >= dl)
                        kputs("[sfs] DDR-967 wait expired; umounting with a probe still live\r\n");
                }

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

                /* DDR-760: persistent SFS root (SFS-as-root half 2/2). The
                 * destructive tests above left blk2 dirty + unmounted; reformat it
                 * CLEAN, remount, provision /etc/aether/config, and root a probe
                 * there (DDR-739 hand-rolled: load blocked, set root_mnt, unblock)
                 * — proving a process can durably root at a clean SFS volume. */
                if (sfs_format(sbd) == 0) {
                    int root_smnt = vfs_mount(2);
                    if (root_smnt >= 0) {
                        /* DDR-761: the FULL AETHER boot policy (mode/task/slot +
                         * the CAP_NET allowlist row) now lives on the SFS root at
                         * /etc/aether/config — the daemon reads it from here. */
                        /* DDR-770: only KERNEL-provision the config when no host
                         * mkfs.sfs image already carries one (prov_mnt < 0). When a
                         * provisioned root is present the daemon roots there
                         * (below) and this write is skipped entirely. */
                        static const char CFGTEXT[] =
                            "mode=sovereign\ntask=test\nslot=0\nnet=10.0.2.2:11434\n";
                        if (prov_mnt < 0) {
                            struct vfs_file cf;
                            if (vfs_create(cap, root_smnt, "/etc/aether/config", &cf) == 0)
                                vfs_write(cap, &cf, 0, (void *)CFGTEXT,
                                          (uint32_t)(sizeof CFGTEXT - 1));
                        }
                        struct tcb *sp = 0;
                        uint64_t splen = (uint64_t)((uintptr_t)sfsroottest_elf_end - (uintptr_t)sfsroottest_elf);
                        if (elf_load((void *)(uintptr_t)sfsroottest_elf, splen,
                                     "SFSROOT", &sp) == ELF_OK && sp) {
                            sp->root_mnt = root_smnt;   /* clean SFS root before unblock */
                            sched_unblock(sp);
                            kputs("[sfs] persistent root provisioned; SFS-rooted probe spawned\r\n");
                        }
                        /* DDR-764: ring-3 large-write probe — writes 8 KiB to the
                         * SFS root (2 extents via the 4 KiB write chunk; the old
                         * 256 B chunk short-wrote at ~1 KiB). */
                        struct tcb *bw = 0;
                        uint64_t bwlen = (uint64_t)((uintptr_t)bigwritetest_elf_end - (uintptr_t)bigwritetest_elf);
                        if (elf_load((void *)(uintptr_t)bigwritetest_elf, bwlen,
                                     "BIGWRITE", &bw) == ELF_OK && bw) {
                            bw->root_mnt = root_smnt;
                            sched_unblock(bw);
                        }

                        /* DDR-761/770: release the AETHER daemon into its root.
                         * If a host mkfs.sfs image provisioned /etc/aether/config
                         * (prov_mnt >= 0), root the daemon there — the config came
                         * from the build image, not the kernel. Otherwise root at
                         * the kernel-provisioned blk2 (DDR-761). */
                        if (dm) {
                            dm->root_mnt = (prov_mnt >= 0) ? prov_mnt : root_smnt;
                            sched_unblock(dm);
                            kputs(prov_mnt >= 0
                                  ? "[sfs] AETHER daemon rooted at provisioned mkfs image\r\n"
                                  : "[sfs] AETHER daemon rooted at SFS /etc/aether/config\r\n");
                        }

                        /* DDR-763: B+tree churn REGRESSION witness. 40 cycles of
                         * create+write(64K)+unlink on the SAME path drive the
                         * inode-entry B+tree well past its first leaf split
                         * (SFS_LEAF_MAX=14) — coverage that no prior gate had, which
                         * is why a write-budget exhaustion (below) was once
                         * mis-attributed to a "B+tree split bug" (it is not — the
                         * tree is sound). The 1 MiB per-thread FS write budget
                         * (vfs.c) is refreshed here so this exercises the B+TREE, not
                         * the budget — this is kernel self-test context, not a
                         * userspace consumer. */
                        {
                            uint64_t cbuf = pmm_alloc_pages(4);   /* 64 KiB */
                            int churn_ok = (cbuf != 0);
                            current_thread->fs_write_budget = ~0ull;   /* test the tree, not the budget */
                            if (cbuf) memset((void *)(uintptr_t)cbuf, 0x5A, 65536);
                            /* BUG-1-SFS (DDR-790 addendum): name WHICH call failed
                             * and at which iteration. The old code collapsed all
                             * three into churn_ok=0, discarding the one fact that
                             * decides whether the SFS allocator is implicated when
                             * the BSP later stops progressing. Failure path only —
                             * no hot-path volume, so it cannot evict gate markers
                             * from the log ring the way DDR-790's traces did. */
                            for (int i = 0; churn_ok && i < 40; i++) {
#if BSP_LIVENESS
                                /* DDR-777/DDR-791: BUG-1 BSP-liveness witness.
                                 *
                                 * The failing runs stop progressing somewhere at
                                 * or after this block, with the timer alive and
                                 * the APs up — but nothing distinguishes "the BSP
                                 * is wedged INSIDE a churn iteration" from "the
                                 * churn finished and the BSP wedged later". One
                                 * marker per iteration settles it: the last
                                 * printed index is where progress stopped.
                                 *
                                 * Opt-in because DDR-790 already proved that
                                 * per-iteration output evicts smoke-dmesg's marker
                                 * from the last-4 KiB log ring. Built only by
                                 * `make smoke-rqstress-liveness`, which restores a
                                 * clean image afterwards. */
                                kputs("[bsp] churn iter=");
                                kputdec((uint64_t)i);
                                kputs(" t=");
                                kputdec(g_ticks);
                                kputs("\r\n");
#endif
                                struct vfs_file cf2;
                                const char *which = 0;
                                /* DDR-884: keep the RETURN CODE. OPEN-10's one
                                 * captured failure is op=create iter=0 — the
                                 * FIRST create, which never reaches a B+tree leaf
                                 * split (SFS_LEAF_MAX=14), so the defect is not
                                 * churn. Which error it is decides the diagnosis:
                                 * -EEXIST means a leftover file or a concurrent
                                 * writer, -ENOSPC means the volume, an ADR-032
                                 * budget refusal means neither. Discarding it left
                                 * all three indistinguishable. */
                                long rc = vfs_create(cap, root_smnt, "/CHURN.TMP", &cf2);
                                if (rc != 0) {
                                    which = "create";
                                } else {
                                    rc = vfs_write(cap, &cf2, 0,
                                                   (void *)(uintptr_t)cbuf, 65536);
                                    if (rc != 65536) {
                                        which = "write";
                                    } else {
                                        rc = vfs_unlink(cap, root_smnt, "/CHURN.TMP");
                                        if (rc != 0)
                                            which = "unlink";
                                    }
                                }
                                if (which) {
                                    kputs("[sfs] churn FAIL op="); kputs(which);
                                    kputs(" iter="); kputdec((uint64_t)i);
                                    kputs(" rc=");
                                    if (rc < 0) { kputs("-"); kputdec((uint64_t)(-rc)); }
                                    else        { kputdec((uint64_t)rc); }
                                    /* DDR-964 §10: rc=-1 is vfs_create's -EPERM
                                     * branch, i.e. THIS handle did not authorize.
                                     * The handle itself says why, and the three
                                     * readings are disjoint: a large index or a
                                     * stale generation means the thread entered
                                     * before ->arg was written (the §5 race);
                                     * h=0 means cap_create returned CAP_NULL
                                     * (§8); a well-formed handle that is still
                                     * refused means neither, and needs its own
                                     * DDR. Failure path only — no hot-path
                                     * volume, so it cannot evict gate markers
                                     * from the log ring the way DDR-790 did. */
                                    kputs(" h="); kputdec((uint64_t)cap);
                                    kputs(" idx="); kputdec((uint64_t)(cap & 0xFFFFFFFFu));
                                    kputs(" gen="); kputdec((uint64_t)(cap >> 32));
                                    kputs(" tid="); kputdec((uint64_t)current_thread->tid);
                                    kputs("\r\n");
                                    churn_ok = 0;
                                }
                            }
                            if (cbuf) pmm_free_pages(cbuf, 4);
                            kputs(churn_ok ? "[sfs] btree churn OK\r\n"
                                           : "[sfs] btree churn FAIL\r\n");
                        }

                        /* DDR-762-v2: free-space GC — observe block REUSE directly
                         * (cheap, ~10 cycles) rather than exhausting the volume (a
                         * 300-cycle loop of incompressible 64K writes was correct but
                         * so slow on TCG it timed OTHER gates' boots out). Each
                         * unlink frees the file's 16-block extent + inode block; with
                         * the free-extent-run allocator the next 64K write reuses the
                         * 16-run (exact fit), so the superblock high-water
                         * (next_free_block) barely advances. WITHOUT reclamation it
                         * would climb ~17 blocks/cycle. We assert the delta over 10
                         * cycles is far below that no-reuse floor (170). Budget
                         * refreshed (DDR-763) so this tests reuse, not the 1 MiB
                         * write budget. INCOMPRESSIBLE fill so LZ4 can't shrink the
                         * extent (a memset would compress to ~1 block). */
                        {
                            uint64_t gbuf = pmm_alloc_pages(4);   /* 64 KiB */
                            int gc_ok = (gbuf != 0);
                            current_thread->fs_write_budget = ~0ull;
                            if (gbuf) {
                                uint8_t *g = (uint8_t *)(uintptr_t)gbuf;
                                uint32_t x = 0x9e3779b9u;
                                for (int k = 0; k < 65536; k++) {
                                    x ^= x << 13; x ^= x >> 17; x ^= x << 5;  /* xorshift */
                                    g[k] = (uint8_t)(x >> 24);
                                }
                            }
                            /* Warm up one cycle so a freed 16-run exists, then sample. */
                            struct vfs_file gf;
                            if (gc_ok &&
                                (vfs_create(cap, root_smnt, "/GC.TMP", &gf) != 0 ||
                                 vfs_write(cap, &gf, 0, (void *)(uintptr_t)gbuf, 65536) != 65536 ||
                                 vfs_unlink(cap, root_smnt, "/GC.TMP") != 0))
                                gc_ok = 0;
                            uint64_t nf0 = sfs_read_next_free(sbd);
                            for (int i = 0; gc_ok && i < 10; i++) {
                                if (vfs_create(cap, root_smnt, "/GC.TMP", &gf) != 0 ||
                                    vfs_write(cap, &gf, 0, (void *)(uintptr_t)gbuf, 65536) != 65536 ||
                                    vfs_unlink(cap, root_smnt, "/GC.TMP") != 0)
                                    gc_ok = 0;
                            }
                            uint64_t nf1 = sfs_read_next_free(sbd);
                            if (gbuf) pmm_free_pages(gbuf, 4);
                            uint64_t grew = (nf1 >= nf0) ? (nf1 - nf0) : ~0ull;
                            kputs("[sfs] free-space GC grew="); kputdec(grew);
                            kputs("\r\n");
                            /* Measured: reclaim ~92, no-reclaim ~262 (10 cycles).
                             * The 16-block data extent + inode are reused each cycle
                             * (exact-fit run), so growth is only leaked btree-CoW
                             * garbage. Threshold 170 discriminates with wide margin
                             * both ways (reclaim << 170 << no-reclaim). */
                            kputs((gc_ok && grew < 170)
                                      ? "[sfs] free-space GC OK\r\n"
                                      : "[sfs] free-space GC FAIL\r\n");

                            /* DDR-889 (item 31): read the PERSISTED free list
                             * back off the DEVICE and require it to be there,
                             * well-formed, and non-empty.
                             *
                             * The obvious test — unmount, remount, watch reuse —
                             * was written first and DOES NOT DISCRIMINATE:
                             * vfs_mount() returns the cached mount, so the
                             * in-memory runs survive and the measurement is the
                             * same whether the on-disk list is loaded or not
                             * (grew=9 loaded vs grew=10 not loaded, measured).
                             * It passed for the wrong reason, so it was removed
                             * rather than tuned into looking decisive.
                             *
                             * This claim is smaller and true: the list reached
                             * the disk, carries its magic, and holds runs. */
                            /* DDR-890 (item 40): PRADYOS Drive. Mounted as a
                             * real VFS volume, so the SAME create/write/read/
                             * unlink/readdir path every disk filesystem uses is
                             * what exercises it — a workspace threaded through
                             * the syscall layer as a special case would prove
                             * nothing about the VFS contract.
                             *
                             * The REFUSAL arms are the point: this is memory an
                             * agent sizes, so both caps must reject rather than
                             * silently truncate. */
                            {
                                int pm = vfs_mount_virtual("pdrive");
                                if (pm < 0) {
                                    kputs("[pdrive] mount FAIL\n");
                                } else {
                                    struct vfs_file pf;
                                    static const char MSG[] = "pradyos drive workspace";
                                    char rb[32];
                                    int ok = 1;
                                    if (vfs_create(cap, pm, "/WORK.TXT", &pf) != 0) ok = 0;
                                    if (ok && vfs_write(cap, &pf, 0, (void *)MSG,
                                                        (uint32_t)(sizeof MSG - 1))
                                              != (int)(sizeof MSG - 1)) ok = 0;
                                    if (ok && vfs_read(cap, &pf, 0, rb,
                                                       (uint32_t)(sizeof MSG - 1))
                                              != (int)(sizeof MSG - 1)) ok = 0;
                                    if (ok && memcmp(rb, MSG, sizeof MSG - 1) != 0) ok = 0;

                                    char nm[64]; uint32_t sz = 0;
                                    int listed = (vfs_readdir(cap, pm, "/", 0, nm, &sz) == 0);

                                    /* Capacity refusal: one write larger than the
                                     * whole volume must be REFUSED, not clipped. */
                                    uint64_t big = pmm_alloc_pages(4);
                                    int refused = 0;
                                    if (big) {
                                        refused = (vfs_write(cap, &pf, 2u*1024*1024,
                                                             (void *)(uintptr_t)big, 4096) < 0);
                                        pmm_free_pages(big, 4);
                                    }
                                    if (ok && vfs_unlink(cap, pm, "/WORK.TXT") != 0) ok = 0;
                                    int gone = (vfs_open(cap, pm, "/WORK.TXT", &pf) != 0);

                                    kputs("[pdrive] mounted id=");
                                    kputdec((uint64_t)pm);
                                    kputs(ok ? " rw OK" : " rw FAIL");
                                    kputs(listed ? " readdir OK" : " readdir FAIL");
                                    kputs(refused ? " overflow REFUSED" : " overflow ACCEPTED");
                                    kputs(gone ? " unlink OK" : " unlink FAIL");
                                    kputs("\n");
                                    kputs((ok && listed && refused && gone)
                                              ? "[pdrive] workspace OK\n"
                                              : "[pdrive] workspace FAIL\n");
                                }
                            }

                            {
                                long fl = sfs_read_freelist_count(sbd);
                                kputs("[sfs] freelist ondisk runs=");
                                if (fl < 0) kputs("none");
                                else        kputdec((uint64_t)fl);
                                kputs("\n");
                                kputs(fl > 0 ? "[sfs] freelist persist OK\n"
                                             : "[sfs] freelist persist FAIL\n");
                            }
                        }
                    }
                }
            } else {
                kputs("[sfs] mount failed\r\n");
            }
        } else {
            kputs("[sfs] format failed\r\n");
        }
    }

    /* ext4 read-only (slice 4j): read a host-written file from the ext4 disk.
     * DDR-739: reuses the single early ext4 mount (no second vfs_mount). */
    if (ext4_mnt >= 0) {
        int emnt = ext4_mnt;
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
    }
}

static void sched_demo(void) {
    uint64_t tsc_hz = calibrate_tsc_hz();
    kputs("NEXUS: TSC ~");
    kputdec(tsc_hz / 1000000);
    kputs(" MHz\r\n");

    sched_init();
    /* DDR-949: sched_create returns NULL on TCB/stack kmalloc failure. Unchecked,
     * bench_ctx_switch below would measure a context switch against a partner
     * that does not exist and report a bogus number as a real measurement. */
    if (!sched_create(bench_partner, 0, "bench"))
        kputs("[bench] FAILED to spawn partner — timing below is not valid\r\n");
    bench_ctx_switch(tsc_hz);
    bench_done = 1;                           /* let the bench thread retire */

    /* Create the demo threads with interrupts masked so each thread's capability
     * (->arg) is fully set before the timer can schedule it. */
    __asm__ volatile("cli");
    ipc_demo();
    ring_demo();
    bus_demo();
    /* DDR-949: same class as the bench spawn above — an unchecked NULL here
     * means the blk self-test silently never runs and its absence reads as
     * "no output", not as "failed to start". */
    if (!sched_create(blk_test_thread, 0, "blk"))
        kputs("[blk] FAILED to spawn blk_test_thread\r\n");
    sched_start_reaper();                     /* 5b-9: reclaim orphaned zombie procs */
    /* DDR-964 §5: this is the OPEN-10 spawn. Created BLOCKED so ->arg holds the
     * capability before any CPU can enter fs_test_thread — the cli above masks
     * only the BSP's timer, and the boot log shows 796 cross-CPU steals. A
     * stolen-early thread entered with ->arg still holding the 0 passed to the
     * create (sched.c:882), so every vfs_create it made returned -EPERM (== -1)
     * from its first one: `[sfs] churn FAIL op=create iter=0 rc=-1`. Reproduced
     * on demand by reverting this line to sched_create (DDR-964 §10). */
    struct tcb *fst = sched_create_blocked(fs_test_thread, 0, "fs");
    if (!fst) {
        kputs("[fs] FAILED to spawn fs_test_thread\r\n");
    } else {
        cap_t fc = cap_create(fst->caps, RES_FILE, FS_RES_ID,
                              CAP_FS_READ | CAP_FS_WRITE |
                              CAP_FS_SFS_READ | CAP_FS_SFS_ADMIN);
        if (fc == CAP_NULL) {
            /* DDR-970: do NOT run it. "every FS op would be -EPERM" understates
             * the damage -- fs_test_thread's self-test block calls
             * sfs_format(sbd) directly on the block device, which is not
             * capability-gated, so a thread without its capability would still
             * reformat the disk while doing nothing useful. Destroy the
             * still-blocked TCB, matching the three sibling DDR-964 sites
             * (main.c:211, 273, 344). */
            kputs("[fs] FAILED to mint fs capability\r\n");
            sched_destroy(fst);
        } else {
            fst->arg = (void *)(uintptr_t)fc;
            sched_unblock(fst);
        }
    }
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
    /* DDR-871 (item 42): probe CPUID for ERMS before anything copies in
     * bulk. Runs on the BSP before APs start, so the flag it writes is
     * read-only by the time any other CPU exists — no locking needed. */
    extern void fast_memcpy_init(void);
    fast_memcpy_init();
    cpu_enable_sse();                    /* PROC-D: x87+SSE for ring-3 C (musl) — ADR-023 §D8 */

    tss_init_cpu(0, 0);                  /* BSP is cpu_idx 0 here; rsp0 set per user thread */
    syscall_init();                      /* EFER.SCE + STAR/LSTAR/SFMASK + dispatch */
    vmm_init();                          /* record kernel master CR3 + enable EFER.NXE (W^X) */
    vmm_protect_kernel();                /* DDR-757: kernel-self W^X (text RX, data NX) —
                                          * before SMP bring-up so APs arm NXE pre-paging */
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
    metric_page_init();                  /* F#68/DDR-795: sealed objective-function root */
    cow_selftest();                      /* IMP-D: copy-on-write fork isolation */

    /* DDR-804: read the boot-time probe list before anything can consult it.
     * Two port reads, no wait, no allocation; fails closed when absent. */
    fwcfg_init();

    /* Phase 3: hardware discovery + first device driver. */
    acpi_init((uint64_t)bi->acpi_rsdp);   /* DDR-978: 0 on the BIOS path */
    numa_init();      /* DDR-882 (item 17a): SRAT topology, needs ACPI first */
    pmm_numa_rebucket();  /* 17b: re-file the free lists onto their nodes */
    /* DDR-882 (item 17b) probe. Parsing SRAT proves nothing about ALLOCATION:
     * a node-aware allocator that ignores its node argument passes every
     * topology assertion. This asks node 1 for a frame and checks the address
     * actually landed inside node 1's range.
     *
     * Opt-in (DDR-804): it allocates and never frees, so running it on every
     * boot would shift free-page counts that other gates assert on. */
    if (probe_enabled("numaalloc")) {
        if (numa_node_count() < 2) {
            kputs("[numa] alloc probe SKIP (single node)\r\n");
        } else {
            uint64_t a = pmm_alloc_pages_node(1, 0);
            uint32_t got = a ? numa_node_of(a) : 0xFFFFFFFFu;
            kputs("[numa] alloc node1 -> node");
            kputdec(got);
            kputs(a && got == 1 ? " OK\r\n" : " MISMATCH\r\n");
            if (a)
                pmm_free_page(a);
        }
    }
    /* DDR-874 (item 28): I/O APIC + q35 GSI routing. Needs the MADT, so it
     * runs straight after acpi_init. Returns 0 on a board without one, and
     * the 8259 PIC stays in charge — this does not disturb the existing
     * routing, it adds the table per-CPU affinity will be expressed in. */
    ioapic_init();
    acpi_power_init();          /* DDR-746: parse FADT + \_S5_ for SYS_POWEROFF */

    /* DDR-892 (item 27). MUST follow acpi_power_init(): that is what scans the
     * DSDT. Reported before it, acpi_s3_available() answered 0 for a machine
     * whose _S3_ the scanner had in fact parsed. The raw-occurrence counter is
     * what separated "the scanner is broken" from "the report ran too early" —
     * two conclusions with opposite fixes. */
    pstate_init();
    kputs(acpi_s3_available() ? "[acpi] S3 available\r\n"
                              : "[acpi] S3 not advertised\r\n");
    acpi_suspend_s3();   /* refuses: discovery ships, entry does not */
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
        /* DDR-875 (item 23): AHCI is class 0x01 subclass 0x06. Matched on
         * BOTH, because class 0x01 alone is "mass storage" and would also
         * catch IDE and NVMe controllers, which this driver cannot drive. */
        if (d->class_code == 0x01 && d->subclass == 0x06)
            ahci_init(d->bus, d->dev, d->func);
        if (d->vendor_id == 0x1AF4 && d->class_code == 0x02)    /* virtio network (NET-A) */
            virtio_net_init(d->bus, d->dev, d->func);
        /* DDR-876 (item 25): Intel e1000e. Matched on vendor+device rather
         * than class 0x02 alone, which is every Ethernet controller — this
         * driver programs 82574L registers and would misdrive anything
         * else that merely claims to be a NIC. */
        if (d->vendor_id == 0x8086 && d->device_id == 0x10D3)
            e1000e_init(d->bus, d->dev, d->func);
        if (d->vendor_id == 0x1AF4 && d->class_code == 0x03)    /* virtio GPU (L7 slice 0) */
            virtio_gpu_init(d->bus, d->dev, d->func);
        /* DDR-816: virtio-rng. Matched on device_id (0x1040 + type 4), not
         * class_code — the 0x1040+type rule is structural to modern virtio,
         * whereas the class byte is not something to assert from memory. */
        if (d->vendor_id == 0x1AF4 &&
            (d->device_id == 0x1044 || d->device_id == 0x1005))
            virtio_rng_init(d->bus, d->dev, d->func);
        if (d->vendor_id == 0x1AF4 && d->class_code == 0x09)    /* virtio input/pointer (DDR-705) */
            virtio_input_init(d->bus, d->dev, d->func);
        if (d->class_code == 0x01 && d->subclass == 0x08)       /* NVMe controller (DDR-765) */
            nvme_init(d->bus, d->dev, d->func);
    }
    /* DDR-972: no real disk means the ISO. Every one of the 147 gates boots
     * through boot_test.sh, which attaches at least one virtio-blk-pci device,
     * so blk_count() is never 0 for any of them and this branch is UNREACHABLE
     * under the gate suite — that is the safety argument for the change, not a
     * hope. The ISO is the only configuration that gets here.
     *
     * DDR-971 measured what happens without it: NEXUS KERNEL OK, then
     * "[blk] no block device" / "[fs] no mountable filesystem found", then
     * rqdepth=1 curpid=0 forever. Give the kernel a memory-backed root and the
     * existing bring-up runs unchanged — fs_test_thread's vfs_mount loop finds
     * it with no edit at all. */
    if (blk_count() == 0) {
        /* Mirror the DISK TOPOLOGY the boot path already expects, rather than
         * relaxing the shared `blk_count() > 2` gate below — that gate is on
         * the path all 147 gates traverse, and editing it to suit the ISO would
         * put every one of them at risk to save three allocations.
         *
         *   blk0  small, left BLANK  — stands in for the boot disk, which is an
         *                              MBR image and is not mountable either;
         *                              fs_test_thread's loop skips it exactly as
         *                              it skips the real one.
         *   blk1  4 MiB, SFS         — the root PRISM and init get.
         *   blk2  4 MiB, left BLANK  — the SFS scratch volume; the existing
         *                              `blk_count() > 2` block formats it itself
         *                              and later reformats it destructively.
         *
         * Order 6 = 256 KiB, order 10 = 4 MiB; ~8.25 MiB total against ~110 MiB
         * free. */
        ramdisk_init(6);                             /* blk0: boot-disk stand-in */
        int rd_root = ramdisk_init(10);              /* blk1: the root           */
        ramdisk_init(10);                            /* blk2: SFS scratch        */
        if (rd_root >= 0) {
            struct blk_device *rbd = blk_get((unsigned)rd_root);
            /* Same call the SFS self-tests already make on blk_get(2). A blank
             * device is what sfs-image ships too ("kernel formats in place"),
             * so this is the established path, not a new one. */
            if (rbd && sfs_format(rbd) == 0)
                kputs("[ramdisk] formatted SFS — ISO root ready\r\n");
            else
                kputs("[ramdisk] SFS format FAILED — root unusable\r\n");
        }
    }

    rng_init();                          /* DDR-816: entropy, fail-closed */
    rng_selftest();                      /* DDR-816: two draws must differ */
    net_init();                          /* NET-B: bring up lwIP over virtio-net */
    aether_init();                       /* Layer 6: PMM-pool queue + audit rings */
    aether_selftest();                   /* Layer 6: smoke-aether-queue (PRADYOS_AETHER_QUEUE_OK) */
    aether_sectest();                    /* Layer 6: smoke-aether-sec (bounds + clean-kill paths) */
    fat32_register();                    /* Phase 4: register the FS driver with the VFS */
    sfs_register();                      /* Phase 4: SOVEREIGN FS (ADR-018) */
    pdrive_register();                   /* DDR-890 (item 40): PRADYOS Drive */
    ext4_register();                     /* Phase 4j: ext4 read-only compat */

    kputs("NEXUS: starting scheduler\r\n");
    sched_demo();                          /* never returns (becomes the idle thread) */
}
