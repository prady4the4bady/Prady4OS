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

void kputc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }   /* wait for THRE */
    outb(COM1, (uint8_t)c);
}

/* COM1 RX ring buffer (5e). An IRQ4 handler drains the UART into this ring from
 * boot, so console input (e.g. a shell's piped command stream) is never lost in
 * the window before a reader runs — the 16-byte UART FIFO would overflow. Single
 * producer (the IRQ) + single consumer (sys_read): head is written only by the
 * IRQ, tail only by the reader, so it is lock-free on this single core. */
#define RX_RING_SZ 256u
static volatile uint8_t  rx_ring[RX_RING_SZ];
static volatile uint32_t rx_head, rx_tail;

/* IRQ4 handler: drain every byte the UART has into the ring (drop on full). */
static void console_rx_irq(void) {
    while (inb(COM1 + 5) & 0x01) {            /* LSR bit0: data ready */
        uint8_t c = inb(COM1);               /* read RBR (also clears the IRQ) */
        uint32_t nh = (rx_head + 1u) % RX_RING_SZ;
        if (nh != rx_tail) {                 /* space available */
            rx_ring[rx_head] = c;
            rx_head = nh;
        }
    }
}

/* Arm COM1 receive: enable the RX FIFO + the Received-Data-Available interrupt,
 * register the IRQ4 handler, and unmask IRQ4 at the PIC. Call once at boot after
 * pic_remap(). */
void console_rx_init(void) {
    outb(COM1 + 2, 0x07);                     /* FCR: enable FIFO + clear RX/TX  */
    outb(COM1 + 1, 0x01);                     /* IER: Received Data Available IRQ */
    irq_register(4, console_rx_irq);          /* COM1 = IRQ4 */
    pic_unmask(4);
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
    for (; *s; ++s)
        kputc(*s);
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

void kvga_line(const char *s, int row) {
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    vga += (uint64_t)row * 80;
    for (int i = 0; s[i]; ++i)
        vga[i] = (uint16_t)((uint8_t)s[i]) | (uint16_t)(0x0F << 8);
}
