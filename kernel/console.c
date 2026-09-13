/* kernel/console.c
 * Minimal ring-0 console: COM1 serial (the smoke test reads this) and the VGA
 * text buffer (for a graphical boot). Once preemptive multitasking is live,
 * multiple threads emit concurrently, so each multi-character write masks
 * interrupts for its duration to stay an atomic unit on this single core —
 * otherwise thread output interleaves mid-line. (A spinlock arrives with SMP.)
 */
#include "console.h"
#include "io.h"
#include "irq.h"            /* irq_register + pic_unmask for COM1 RX (5e) */

#define COM1 0x3F8

/* ADR-030 stage 1: the ADR-016 masking helpers now acquire the console
 * spinlock (irqsave — one-CPU semantics unchanged, cross-CPU exclusion added
 * for the ADR-029 APs). Call sites unchanged. */
#include "spinlock.h"
static spinlock_t g_console_lock = SPINLOCK_INIT;

/* DDR-963 §5. Distinct from g_console_lock and always taken OUTSIDE it. */
static spinlock_t g_line_lock = SPINLOCK_INIT;

uint64_t console_line_lock(void) {
    return spin_lock_irqsave(&g_line_lock);
}

/* Masks interrupts first, then tries once. On failure interrupts are restored
 * and the caller prints anyway — a spliced line is a cosmetic loss, whereas a
 * trap printer that blocks turns a diagnosable fault into a hang. */
int console_line_trylock(uint64_t *fl) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    if (spin_trylock(&g_line_lock)) { *fl = f; return 1; }
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
    return 0;
}

void console_line_unlock(uint64_t fl) {
    spin_unlock_irqrestore(&g_line_lock, fl);
}

/* DDR-970: for the kernel-panic path ONLY. A ring-0 fault taken inside a
 * line-locked region skips the ring-3 trylock branch and halts in `cli; hlt`
 * still holding g_line_lock, so every other CPU's next console_line_lock()
 * spins forever with interrupts already masked -- a diagnosable panic becomes a
 * silent whole-machine hang.
 *
 * Releasing a lock the caller may not hold is defensible here and nowhere else:
 * the path is terminal, nothing after it depends on the lock's integrity, and
 * the lock protects cosmetic line atomicity only. No flag restore -- a panicking
 * CPU keeps interrupts masked. */
/* DDR-1009: RELEASE BOTH LOCKS, and the name now says so.
 *
 * DDR-970's reasoning above is correct and was applied to exactly one lock.
 * `kputs` -- which is what the panic printer itself calls -- does NOT take
 * g_line_lock. It takes g_console_lock (declared 3 lines apart from it, and
 * explicitly "Distinct from g_console_lock") for the whole string via
 * irq_save()/irq_restore(). So nothing force-released the lock the panic path
 * actually needs.
 *
 * DDR-979's one-winner latch then made that strictly worse: a CPU that LOSES the
 * latch halts in `for(;;) cli; hlt` (idt.c:697) still holding whatever it held.
 * If it was faulted out of a kputs, that is g_console_lock -- and the winner's
 * next kputs spins on it forever with interrupts already masked. Measured
 * artefact (DDR-1009 §2): CI run on 81274f4, shard 6, smoke-msixap printed
 * "*** NEXUS KERNEL PANIC ***" and then not one further byte for the rest of the
 * gate's window. idt.c:702 is the very next statement.
 *
 * This is called BEFORE the latch (idt.c:673), so one edit covers both the
 * winner and the loser.
 *
 * The cost is the same one DDR-970 already accepted and is worth restating:
 * releasing a lock another CPU may legitimately hold lets its output interleave
 * with the dump. §INV.23 already requires every reader to reconstruct panic
 * fields BY NAME rather than by line position, for exactly this reason. A
 * garbled dump beats no dump and a hung machine. */
void console_panic_force_release(void) {
    spin_unlock(&g_line_lock);
    spin_unlock(&g_console_lock);
}
static inline uint64_t irq_save(void) {
    return spin_lock_irqsave(&g_console_lock);
}
static inline void irq_restore(uint64_t f) {
    spin_unlock_irqrestore(&g_console_lock, f);
}

/* DDR-750: kernel log ring. Every byte emitted through kputc is captured into a
 * fixed circular buffer so a running program can read the log back (SYS_DMESG).
 * A dedicated leaf spinlock guards it — always taken innermost (the console lock,
 * when held by the bulk printers, nests outside it), so there is no deadlock; the
 * per-char lock is negligible against kputc's UART busy-wait below. */
#define KLOG_SZ 8192u
static char        klog_buf[KLOG_SZ];
static uint32_t    klog_head;                  /* next write position           */
static uint64_t    klog_total;                 /* total bytes ever written       */
static spinlock_t  klog_lock = SPINLOCK_INIT;

static void klog_putc(char c) {
    uint64_t fl = spin_lock_irqsave(&klog_lock);
    klog_buf[klog_head] = c;
    klog_head = (klog_head + 1u) % KLOG_SZ;
    klog_total++;
    spin_unlock_irqrestore(&klog_lock, fl);
}

/* Copy the most-recent min(max, available) bytes of the log, oldest-to-newest,
 * into the kernel buffer `dst`. Returns the byte count. Lock held only around the
 * in-kernel copy — the caller copyouts to user space afterwards (never under the
 * lock). */
uint32_t klog_read(char *dst, uint32_t max) {
    uint64_t fl = spin_lock_irqsave(&klog_lock);
    uint32_t avail = (klog_total < KLOG_SZ) ? (uint32_t)klog_total : KLOG_SZ;
    uint32_t n     = (avail < max) ? avail : max;
    uint32_t oldest = (klog_total < KLOG_SZ) ? 0u : klog_head;  /* ring-full: oldest at head */
    uint32_t pos    = (oldest + (avail - n)) % KLOG_SZ;         /* keep most-recent n bytes   */
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = klog_buf[pos];
        pos = (pos + 1u) % KLOG_SZ;
    }
    spin_unlock_irqrestore(&klog_lock, fl);
    return n;
}

/* DDR-809: bound for the THRE wait. Far beyond any real transmit-holding latency
 * (QEMU sets the bit within a handful of reads), but a hard ceiling — the
 * unbounded form was an S2 violation that, on the BSP, would have taken g_ticks
 * with it and silently un-bounded every g_ticks-deadline wait in the tree. */
#define CONSOLE_THRE_MAX 10000u

static void console_rx_drain(void);           /* defined with the IRQ4 handler */

/* TEMP DIAG (DDR-916 arm4): count characters dropped by the THRE ceiling.
 * Reported from the heartbeat (idt.c) rather than printed here — printing from
 * inside kputc would recurse. Confirms or refutes the ceiling hypothesis before
 * any fix is written. */
uint64_t g_thre_drops;

/* TEMP DIAG (DDR-916 PHASE A): count bytes discarded because the RX ring was
 * FULL. Distinguishes ring overflow (this counter rises => capacity is the
 * constraint, arm3) from UART-FIFO overflow (both counters stay 0 => bytes are
 * lost before they ever reach the ring, arm5). */
uint64_t g_rx_drops;
static volatile int g_rx_armed;               /* set by console_rx_init */

void kputc(char c) {
    klog_putc(c);                             /* DDR-750: capture into the log ring */
    unsigned spins = 0;
    while ((inb(COM1 + 5) & 0x20) == 0) {     /* wait for THRE */
        /* DDR-809/DDR-808: kputs/kwrite hold the console lock with interrupts
         * OFF for the whole buffer, so IRQ4 cannot run and COM1's 16-byte RX
         * FIFO cannot be drained by its handler. A burst of output therefore
         * destroyed concurrent console input — measured as one lost command per
         * smoke-shell run. Draining inline here is what closes that window;
         * the masking itself must stay, because ADR-030 needs the buffer atomic. */
        if (g_rx_armed)
            console_rx_drain();
        if (++spins >= CONSOLE_THRE_MAX) {
            g_thre_drops++;                   /* TEMP DIAG: is the ceiling real? */
            return;                           /* bounded (S2); klog already has it */
        }
    }
    outb(COM1, (uint8_t)c);
    /* DDR-916 arm2 (per-character drain here) was tested TWICE and reverted both
     * times. The second test was the valid one: run against deterministic
     * post-b08058b runs and scored on MARKER PRESENCE rather than serial line
     * counts, it changed nothing — 3/3 runs still lost BIGTAIL and EAP55a at
     * exactly the same point. Drain FREQUENCY is not the constraint; ring
     * CAPACITY is (see RX_RING_SZ). Do not re-add without new evidence. */
}

/* COM1 RX ring buffer (5e). An IRQ4 handler drains the UART into this ring from
 * boot, so console input (e.g. a shell's piped command stream) is never lost in
 * the window before a reader runs — the 16-byte UART FIFO would overflow. Single
 * producer (the IRQ) + single consumer (sys_read): head is written only by the
 * IRQ, tail only by the reader, so it is lock-free on this single core. */
/* DDR-916 arm3: sized from a MEASURED overflow, not chosen.
 *
 * A ring-3 reader can stop consuming console input for long stretches while the
 * kernel prints (kputs/kwrite hold the console lock with IRQs off for a whole
 * buffer, so IRQ4 cannot retire the UART FIFO for the duration). The feeder
 * keeps writing throughout. With a 256-byte ring, console_rx_drain's full-branch
 * discarded 102 bytes in a single smoke-shell run (measured via g_rx_drops,
 * reported in the heartbeat; g_thre_drops stayed 0 over the same run, so the
 * loss is capacity, not the TX spin bound).
 *
 * Those discards are what fuse feeder commands: the tail of one command is
 * dropped and the next concatenates onto the orphan ("agent spawn" + "fg %1"
 * arriving as "agenfg"). 4096 is ~40x the measured worst-case overflow and
 * costs 4 KiB of BSS. */
#define RX_RING_SZ 4096u
static volatile uint8_t  rx_ring[RX_RING_SZ];
static volatile uint32_t rx_head, rx_tail;

/* DDR-809: the producer side is no longer lock-free. Two producers now exist —
 * the IRQ4 handler and kputc's in-spin drain — so rx_head is serialised. The
 * CONSUMER side is unchanged: kgetc_nb() is still the only reader and still
 * needs no lock.
 *
 * S6: this takes a lock from ISR context, which the rule restricts to patterns
 * already in the tree. It is one — klog_putc() takes klog_lock via
 * spin_lock_irqsave and kputc calls it on every character, including from ISRs.
 * g_rx_lock is a leaf: nothing is acquired under it. Lock order where both are
 * held is g_console_lock -> g_rx_lock (kputc holds the console lock when it
 * drains); the ISR takes only g_rx_lock, so there is no inversion. */
static spinlock_t g_rx_lock = SPINLOCK_INIT;

/* Drain every byte the UART has into the ring (drop on full). Called from the
 * IRQ4 handler AND from kputc's THRE spin, where IRQ4 cannot fire. */
static void console_rx_drain(void) {
    uint64_t fl = spin_lock_irqsave(&g_rx_lock);
    while (inb(COM1 + 5) & 0x01) {            /* LSR bit0: data ready */
        uint8_t c = inb(COM1);               /* read RBR (also clears the IRQ) */
        uint32_t nh = (rx_head + 1u) % RX_RING_SZ;
        if (nh != rx_tail) {                 /* space available */
            rx_ring[rx_head] = c;
            rx_head = nh;
        } else {
            g_rx_drops++;                    /* TEMP DIAG: ring full, byte lost */
        }
    }
    spin_unlock_irqrestore(&g_rx_lock, fl);
}

/* IRQ4 handler. */
static void console_rx_irq(void) {
    console_rx_drain();
}

/* Arm COM1 receive: enable the RX FIFO + the Received-Data-Available interrupt,
 * register the IRQ4 handler, and unmask IRQ4 at the PIC. Call once at boot after
 * pic_remap(). */
void console_rx_init(void) {
    outb(COM1 + 2, 0x07);                     /* FCR: enable FIFO + clear RX/TX  */
    outb(COM1 + 1, 0x01);                     /* IER: Received Data Available IRQ */
    irq_register(4, console_rx_irq);          /* COM1 = IRQ4 */
    pic_unmask(4);
    /* DDR-809: only now are the RX FIFO and IER configured, so only now is an
     * inline drain meaningful. A plain global with no percpu/LAPIC dependency —
     * kputc prints the earliest boot messages, long before this_cpu() or
     * lapic_id() are usable, so any CPU-identity guard here would fault on the
     * first character the kernel ever prints. */
    g_rx_armed = 1;
}

/* Non-blocking console read (5e): one buffered byte, or -1 if the ring is empty.
 * The blocking/line discipline lives in sys_read(FD_CONSOLE), which polls this
 * and yields while waiting — see ADR-024 §D1. */
int kgetc_nb(void) {
    if (rx_tail == rx_head)
        return -1;                            /* empty */
    uint8_t c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1u) % RX_RING_SZ;
    return (int)c;
}

void kputs(const char *s) {
    uint64_t fl = irq_save();
    /* DDR-916 arm1-ext: burst-start drain. kputs holds irq_save() across the
     * whole buffer, so IRQ4 cannot retire RX during it; drain once up front to
     * close the window between irq_save() and the first character's outb.
     * console_rx_drain() BUFFERS into rx_ring — never discard (see 3065e78). */
    if (g_rx_armed)
        console_rx_drain();
    for (; *s; ++s)
        kputc(*s);
    irq_restore(fl);
}

/* Emit `n` bytes as a single locked unit — the console lock is taken once for
 * the whole buffer, so a user sys_write can't interleave mid-string with a
 * kernel kputs or with another CPU's write (ADR-030: matters once APs run ring-3
 * printers concurrently with the BSP — DDR-SMP-rq-3). */
void kwrite(const char *buf, uint64_t n) {
    uint64_t fl = irq_save();
    /* DDR-916 arm1-ext: burst-start drain — same rationale as kputs. */
    if (g_rx_armed)
        console_rx_drain();
    for (uint64_t i = 0; i < n; ++i)
        kputc(buf[i]);
    irq_restore(fl);
}

void kputhex(uint64_t v) {
    static const char digits[] = "0123456789ABCDEF";
    uint64_t fl = irq_save();
    kputc('0');
    kputc('x');
    for (int shift = 60; shift >= 0; shift -= 4)
        kputc(digits[(v >> shift) & 0xF]);
    irq_restore(fl);
}

void kputdec(uint64_t v) {
    char buf[20];
    int i = 0;
    uint64_t fl = irq_save();
    if (v == 0) {
        kputc('0');
        irq_restore(fl);
        return;
    }
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i)
        kputc(buf[--i]);
    irq_restore(fl);
}

/* ---- DDR-1055: single-call line assembly -------------------------------
 *
 * A logical line built from several kputs/kputdec/kputhex calls is NOT atomic,
 * and console_line_lock() does not make it so: it excludes only OTHER holders
 * of g_line_lock, and the busiest printer in this system -- kwrite, the ring-3
 * write(2) path -- never took it.  A ring-3 probe on another CPU could
 * therefore land its whole line between two of ours, and did:
 *
 *     [user] ELF loaded (embedded); net hammer spawned=PRADYOS_SOVEGRESS_AUDITED
 *
 * every character the kernel meant to print, in a line no gate can match.
 *
 * Assemble here and emit with ONE kwrite.  kwrite holds g_console_lock for the
 * whole buffer, and EVERY printer in the tree takes that lock, so the result is
 * atomic against all of them -- which is a strictly stronger guarantee than the
 * line lock can give, and needs no new locking discipline at the call site.
 *
 * Overflow is LOUD, never silent: a truncated sentinel is the same failure this
 * exists to remove, so it emits its own '[kline] TRUNC' line, which is in
 * GLOBAL_FORBIDDEN.  KLINE_MAX is 256 against a longest current user of ~60. */
void kline_init(kline *k) { k->n = 0; k->trunc = 0; }

void kline_c(kline *k, char c) {
    if (k->n + 1u >= (unsigned)KLINE_MAX) { k->trunc = 1; return; }
    k->b[k->n++] = c;
}

void kline_s(kline *k, const char *s) {
    if (!s) s = "(null)";
    while (*s) kline_c(k, *s++);
}

void kline_d(kline *k, uint64_t v) {
    char t[20];
    int i = 0;
    if (v == 0) { kline_c(k, '0'); return; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) kline_c(k, t[--i]);
}

void kline_x(kline *k, uint64_t v) {
    static const char digits[] = "0123456789ABCDEF";
    kline_c(k, '0'); kline_c(k, 'x');
    for (int shift = 60; shift >= 0; shift -= 4)
        kline_c(k, digits[(v >> shift) & 0xF]);
}

void kline_emit(kline *k) {
    if (k->n) kwrite(k->b, k->n);
    if (k->trunc) kwrite("[kline] TRUNC\r\n", 15);
    k->n = 0; k->trunc = 0;
}

void kvga_line(const char *s, int row) {
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    vga += (uint64_t)row * 80;
    for (int i = 0; s[i]; ++i)
        vga[i] = (uint16_t)((uint8_t)s[i]) | (uint16_t)(0x0F << 8);
}
