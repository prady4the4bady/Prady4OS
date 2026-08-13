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
    uint32_t oldest = (klog_total < KLOG_SZ)
                      ? 0u
                      : (uint32_t)(klog_head);
    for (uint32_t i = 0; i < n; i++)
        dst[i] = klog_buf[(oldest + i) % KLOG_SZ];
    spin_unlock_irqrestore(&klog_lock, fl);
    return n;
}

/* ---------------------------------------------------------------------------
 * COM1 UART helpers
 * ---------------------------------------------------------------------------*/

/* DDR-807: poll THRE (bit 5 of LSR) before each outb so we never overrun the
 * 16-byte FIFO when the host is slower than the guest. */
static inline void uart_wait_tx(void) {
    while (!(inb(COM1 + 5) & 0x20))
        asm volatile("pause");
}

/* DDR-916: drain any RX bytes that have arrived in the UART FIFO.
 * Called (a) after each outb in kputc so residual RX from a host burst is
 * retired one character at a time, and (b) at the entry of kputs/kwrite
 * BEFORE the character loop so the burst-start window is closed.
 *
 * Placement rationale:
 *   Per-character drain alone (original DDR-916 partial fix) was insufficient
 *   because kputs/kwrite hold irq_save() across the whole buffer.  Between
 *   the klog_putc overhead and the inb-LSR poll, the 16-byte FIFO can fill
 *   before the first character's outb when the host delivers a full command
 *   line at once.  Adding a burst-entry drain closes that window.
 *
 *   Both placements are therefore required:
 *     1. kputc   — per-character drain (original fix, still present below)
 *     2. kputs/kwrite entry — burst-start drain (this extension)
 */
static inline void uart_drain_rx(void) {
    /* LSR bit 0 (DR) set means a byte is waiting in RBR. Read and discard. */
    while (inb(COM1 + 5) & 0x01)
        (void)inb(COM1);
}

/* ---------------------------------------------------------------------------
 * VGA helpers
 * ---------------------------------------------------------------------------*/
#define VGA_BASE  ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_ATTR  0x0700   /* light grey on black */

static int vga_row;
static int vga_col;

static void vga_scroll(void) {
    for (int r = 1; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_BASE[(r-1)*VGA_COLS + c] = VGA_BASE[r*VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        VGA_BASE[(VGA_ROWS-1)*VGA_COLS + c] = VGA_ATTR | ' ';
    vga_row = VGA_ROWS - 1;
}

static void vga_putc_raw(char c) {
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
    } else if (c == '\r') {
        vga_col = 0;
    } else {
        VGA_BASE[vga_row * VGA_COLS + vga_col] = VGA_ATTR | (uint8_t)c;
        if (++vga_col >= VGA_COLS) {
            vga_col = 0;
            if (++vga_row >= VGA_ROWS) vga_scroll();
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

void console_init(void) {
    /* Disable interrupts, set 38400 baud (divisor=3), 8N1, enable FIFO. */
    outb(COM1 + 1, 0x00);   /* disable interrupts          */
    outb(COM1 + 3, 0x80);   /* DLAB on                     */
    outb(COM1 + 0, 0x03);   /* divisor low  (38400 baud)   */
    outb(COM1 + 1, 0x00);   /* divisor high                */
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB off               */
    outb(COM1 + 2, 0xC7);   /* enable FIFO, 14-byte thresh */
    outb(COM1 + 4, 0x0B);   /* RTS+DTR+OUT2                */
    vga_row = 0; vga_col = 0;
}

void kputc(char c) {
    uint64_t f = irq_save();
    klog_putc(c);
    uart_wait_tx();
    outb(COM1, (uint8_t)c);
    /* DDR-916 arm0: per-character RX drain — retires bytes that arrived
     * between the previous outb and this one. */
    uart_drain_rx();
    vga_putc_raw(c);
    irq_restore(f);
}

void kputs(const char *s) {
    uint64_t f = irq_save();
    /* DDR-916 arm1-ext: burst-start drain — retire any RX bytes that
     * accumulated before this burst begins, closing the window between
     * irq_save() and the first character's outb. */
    uart_drain_rx();
    for (; *s; s++) {
        klog_putc(*s);
        uart_wait_tx();
        outb(COM1, (uint8_t)*s);
        uart_drain_rx();   /* per-character drain (arm0) still active */
        vga_putc_raw(*s);
    }
    irq_restore(f);
}

void kwrite(const char *buf, uint32_t len) {
    uint64_t f = irq_save();
    /* DDR-916 arm1-ext: burst-start drain — same rationale as kputs above. */
    uart_drain_rx();
    for (uint32_t i = 0; i < len; i++) {
        klog_putc(buf[i]);
        uart_wait_tx();
        outb(COM1, (uint8_t)buf[i]);
        uart_drain_rx();   /* per-character drain (arm0) still active */
        vga_putc_raw(buf[i]);
    }
    irq_restore(f);
}

void kprintf(const char *fmt, ...) {
    /* Minimal kprintf: %s %d %u %x %c %% only. No heap, no floats. */
    va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { kputc(*p); continue; }
        switch (*++p) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    kputs(s); break; }
        case 'd': { int v = __builtin_va_arg(ap, int);
                    if (v < 0) { kputc('-'); v = -v; }
                    /* fall through to unsigned */ }
        /* FALLTHROUGH */
        case 'u': { unsigned v = __builtin_va_arg(ap, unsigned);
                    char tmp[12]; int i = 0;
                    if (!v) { kputc('0'); break; }
                    while (v) { tmp[i++] = '0' + v % 10; v /= 10; }
                    while (i--) kputc(tmp[i]); break; }
        case 'x': { unsigned v = __builtin_va_arg(ap, unsigned);
                    char tmp[9]; int i = 0;
                    if (!v) { kputc('0'); break; }
                    while (v) { int d = v & 0xf;
                                tmp[i++] = d < 10 ? '0'+d : 'a'+d-10;
                                v >>= 4; }
                    while (i--) kputc(tmp[i]); break; }
        case 'c': kputc((char)__builtin_va_arg(ap, int)); break;
        case '%': kputc('%'); break;
        default:  kputc('%'); kputc(*p); break;
        }
    }
    __builtin_va_end(ap);
}
