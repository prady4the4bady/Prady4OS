/* kernel/drivers/rng/virtio_rng.c — virtio-rng + CPU entropy, DDR-816.
 *
 * virtio-rng is the simplest virtio device there is: one virtqueue, and the
 * driver posts a device-WRITABLE buffer which the device fills with entropy.
 * The transport is already generic and gate-proven across blk/net/gpu/input, so
 * this is a new consumer of existing infrastructure rather than new transport
 * code.
 *
 * Device identity: modern virtio PCI device IDs are 0x1040 + <virtio device
 * type>, and virtio-rng is type 4, hence 0x1044 (0x1005 transitional). Matching
 * on device_id rather than class_code is deliberate — the class byte QEMU
 * assigns to virtio-rng is not something this file should assert from memory,
 * whereas the 0x1040+type convention is structural to modern virtio. If the ID
 * were wrong the device is simply never found, rng_source() reports NONE, and
 * smoke-rng fails loudly. The gate verifies the constant; this comment does not
 * have to be trusted. (First implementation attempt confirmed it empirically:
 * smoke-rng passed.)
 */
#include "rng.h"
#include "virtio_pci.h"
#include "virtio.h"
#include "pcie.h"
#include "console.h"
#include "string.h"
#include "pmm.h"

#define RNG_QUEUE_SIZE 8u
#define RNG_BUF_LEN    64u                 /* one draw; callers loop for more */

static struct virtio_pci_dev g_rdev;
static struct virtq          g_rq;
static unsigned              g_source = RNG_NONE;
static uint64_t              g_buf_phys;   /* identity-mapped bounce page */

/* ---- CPU source (x86_64 only) ------------------------------------------- */
#if defined(__x86_64__)
static int cpu_has_rdseed(void) {
    uint32_t a = 7, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (b >> 18) & 1u;                 /* CPUID.(EAX=7,ECX=0):EBX[18] */
}
static int cpu_rdseed64(uint64_t *out) {
    unsigned char ok;
    /* Bounded retry (S2): RDSEED legitimately fails under load when the
     * hardware pool is drained, and must not spin forever. */
    for (unsigned i = 0; i < 10u; i++) {
        __asm__ volatile("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok));
        if (ok)
            return 0;
    }
    return -1;
}
#endif

/* ---- virtio-rng --------------------------------------------------------- */
static int virtio_rng_probe(uint8_t bus, uint8_t dev, uint8_t func) {
    if (virtio_pci_attach(&g_rdev, bus, dev, func) != 0)
        return -1;
    (void)virtio_pci_negotiate(&g_rdev, 0);       /* rng defines no features */
    if (virtq_alloc(&g_rq, RNG_QUEUE_SIZE) != 0)
        return -1;
    if (virtio_pci_setup_queue(&g_rdev, &g_rq, 0) != 0)
        return -1;
    virtio_pci_driver_ok(&g_rdev);

    g_buf_phys = pmm_alloc_page();
    if (!g_buf_phys)
        return -1;
    g_source = RNG_VIRTIO;
    return 0;
}

void virtio_rng_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_source == RNG_VIRTIO)
        return;                                   /* first device wins */
    if (virtio_rng_probe(bus, dev, func) == 0)
        kputs("[rng] virtio-rng ready\r\n");
    else
        kputs("[rng] virtio-rng probe FAILED\r\n");
}

/* Draw up to RNG_BUF_LEN bytes from the device. Returns bytes obtained, or -1. */
static int virtio_rng_draw(uint8_t *out, uint32_t want) {
    if (g_source != RNG_VIRTIO || !g_buf_phys)
        return -1;
    if (want > RNG_BUF_LEN)
        want = RNG_BUF_LEN;

    uint8_t *buf = (uint8_t *)(uintptr_t)g_buf_phys;
    memset(buf, 0, want);            /* so a device that writes nothing is visible */

    struct virtq_buf b = { .phys = g_buf_phys, .len = want, .write = 1 };
    int head = virtq_add(&g_rq, &b, 1);
    if (head < 0)
        return -1;
    virtq_publish(&g_rq, head);
    virtio_pci_notify(&g_rdev, &g_rq, 0);

    /* Bounded poll (S2): the device is synchronous in practice, but an
     * unbounded wait would hang the boot on a misbehaving device. */
    uint32_t used_len = 0;
    int got = -1;
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        got = virtq_pop_used(&g_rq, &used_len);
        if (got >= 0)
            break;
    }
    if (got < 0)
        return -1;
    virtq_free_chain(&g_rq, got);

    if (used_len == 0 || used_len > want)
        return -1;
    for (uint32_t i = 0; i < used_len; i++)
        out[i] = buf[i];
    return (int)used_len;
}

/* ---- public API --------------------------------------------------------- */
int rng_init(void) {
    if (g_source == RNG_VIRTIO) {
        kputs("[rng] source=virtio\r\n");
        return 0;
    }
#if defined(__x86_64__)
    if (cpu_has_rdseed()) {
        g_source = RNG_CPU;
        kputs("[rng] source=cpu (RDSEED)\r\n");
        return 0;
    }
#endif
    /* Fail closed, and say so. A machine running without entropy must announce
     * it on every boot rather than discovering it when a key is needed. */
    kputs("[rng] source=NONE - crypto features will refuse to start\r\n");
    return -1;
}

/* DDR-816 self-test. A gate cannot prove randomness; what it CAN prove is that
 * the source is not returning a fixed buffer — the likeliest real bug is a
 * driver that appears to work and hands back the same page every time.
 *
 * Kernel-side rather than a ring-3 probe because DDR-816 specifies no syscall,
 * and exposing entropy to user space is a separate feature with its own design
 * questions (who may call it, at what rate). */
void rng_selftest(void) {
    if (g_source == RNG_NONE) {
        /* Correct behaviour with no device attached — this IS the fail-closed
         * path, which every non-rng gate exercises. Not a failure. */
        kputs("[rng] selftest skipped: no source\r\n");
        return;
    }
    uint8_t a[32], b[32];
    if (rng_bytes(a, sizeof a) != 0 || rng_bytes(b, sizeof b) != 0) {
        kputs("RNG FAIL: rng_bytes failed with a source present\r\n");
        return;
    }
    unsigned same = 1;
    for (unsigned i = 0; i < sizeof a; i++)
        if (a[i] != b[i]) { same = 0; break; }
    if (same) {
        kputs("PRADYOS_RNG_STUB\r\n");   /* fixed buffer: not entropy */
        return;
    }
    kputs("PRADYOS_RNG_OK\r\n");
}

unsigned rng_source(void) { return g_source; }

int rng_bytes(void *out, uint32_t len) {
    uint8_t *p = (uint8_t *)out;
    if (!p || len == 0)
        return -1;

    if (g_source == RNG_VIRTIO) {
        uint32_t done = 0;
        while (done < len) {
            int n = virtio_rng_draw(p + done, len - done);
            if (n <= 0)
                return -1;
            done += (uint32_t)n;
        }
        return 0;
    }
#if defined(__x86_64__)
    if (g_source == RNG_CPU) {
        uint32_t done = 0;
        while (done < len) {
            uint64_t v;
            if (cpu_rdseed64(&v) != 0)
                return -1;
            uint32_t take = (len - done < 8u) ? (len - done) : 8u;
            for (uint32_t i = 0; i < take; i++)
                p[done + i] = (uint8_t)(v >> (i * 8));
            done += take;
        }
        return 0;
    }
#endif
    return -1;                       /* RNG_NONE: no source, no bytes */
}
