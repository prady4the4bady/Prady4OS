/* kernel/arch/riscv64/start.c — riscv64 boot-to-console (ADR-034).
 *
 * Reaches C from OpenSBI's S-mode handoff, brings up the NS16550A console,
 * prints the sentinel, halts. Scope is BOOT ONLY (ADR-034 decision 3).
 *
 * NS16550A on QEMU `virt`:
 *   base 0x1000_0000 · THR at 0x00 · LSR at 0x05 · LSR bit 5 (0x20) = THR empty
 *
 * Direct MMIO rather than the SBI console call: the UART is a documented part
 * of the `virt` machine and writing it keeps the console independent of which
 * SBI extensions the firmware happens to implement. If a later platform has no
 * NS16550A, an SBI path is the fallback — not the other way round.
 */

#include <stdint.h>

#define UART_BASE      0x10000000UL
#define UART_THR       0x00
#define UART_LSR       0x05
#define UART_LSR_THRE  0x20u       /* transmit holding register empty */

static inline volatile uint8_t *ns16550(unsigned long offset) {
    return (volatile uint8_t *)(UART_BASE + offset);
}

static void uart_putc(char c) {
    /* Wait for room rather than dropping — a dropped byte corrupts the sentinel
     * and turns "console is slow" into "kernel did not boot". */
    while ((*ns16550(UART_LSR) & UART_LSR_THRE) == 0) {
        /* spin */
    }
    *ns16550(UART_THR) = (uint8_t)c;
}

static void uart_puts(const char *s) {
    for (; *s; s++) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
    }
}

/* Called from boot.S on hart 0 with a valid stack and a zeroed BSS. */
void arch_main(void);
void arch_main(void) {
    uart_puts("PRADYOS BOOT OK\n");
    uart_puts("NEXUS: entered arch_main (riscv64, S-mode, MMU off)\n");
    uart_puts("NEXUS KERNEL OK\n");
    uart_puts("NEXUS: riscv64 bootstrap complete — boot-only slice (ADR-034)\n");

    for (;;) {
        __asm__ volatile("wfi");
    }
}
