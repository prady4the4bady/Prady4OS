/* kernel/arch/aarch64/start.c — aarch64 boot-to-console (ADR-034).
 *
 * Reaches C, brings up the PL011 console, prints the sentinel the gate greps
 * for, and halts. Scope is deliberately BOOT ONLY — see ADR-034 decision 3; the
 * PMM/VMM/scheduler/VFS gates are x86_64-specific today and each needs its own
 * slice.
 *
 * PL011 on QEMU `virt` (ADR-034 records these as platform facts, so a later
 * reader can check them rather than trust them):
 *   base 0x0900_0000 · DR at 0x00 · FR at 0x18 · FR bit 5 = TXFF (TX FIFO full)
 *
 * No MMU, no caches: the reset attributes are used as-is, which is correct for
 * device MMIO and explicitly not a foundation for the memory work to come.
 */

#include <stdint.h>

#define PL011_BASE   0x09000000UL
#define PL011_DR     0x00
#define PL011_FR     0x18
#define PL011_FR_TXFF (1u << 5)

static inline volatile uint32_t *pl011(unsigned long offset) {
    return (volatile uint32_t *)(PL011_BASE + offset);
}

static void uart_putc(char c) {
    /* Busy-wait on the FIFO rather than dropping: a dropped byte here corrupts
     * the sentinel and the gate reports "kernel did not boot", which is a very
     * different investigation from "the console is slow". */
    while (*pl011(PL011_FR) & PL011_FR_TXFF) {
        /* spin */
    }
    *pl011(PL011_DR) = (uint32_t)(unsigned char)c;
}

static void uart_puts(const char *s) {
    for (; *s; s++) {
        if (*s == '\n')
            uart_putc('\r');       /* the gate reads a raw serial capture */
        uart_putc(*s);
    }
}

/* CurrentEL[3:2] holds the exception level. ADR-034 uncertainty (1) asks which
 * level QEMU's `-kernel` path actually enters at; printing the assumption would
 * have answered nothing, so this reads the register and prints what it finds. */
static unsigned current_el(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (unsigned)((el >> 2) & 3u);
}

/* Called from boot.S with a valid stack and a zeroed BSS. */
void arch_main(void);
void arch_main(void) {
    uart_puts("PRADYOS BOOT OK\n");
    uart_puts("NEXUS: entered arch_main (aarch64, EL");
    uart_putc((char)('0' + current_el()));
    uart_puts(", MMU off)\n");
    uart_puts("NEXUS KERNEL OK\n");
    uart_puts("NEXUS: aarch64 bootstrap complete — boot-only slice (ADR-034)\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
