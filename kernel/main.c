/* kernel/main.c
 * ===========================================================================
 * NEXUS kernel — minimal long-mode entry point (Phase 2a, first slice).
 *
 * This is reached in 64-bit long mode, ring 0, with a flat identity map of the
 * low 1 GiB set up by the bootloader (boot/stage2). It proves the boot -> 64-bit
 * C kernel handoff works. There is no IDT, no paging of our own, no allocator
 * yet — those are subsequent Phase 2a/2b slices. The only outputs are COM1
 * serial (for the smoke test) and the VGA text buffer (for a graphical boot).
 *
 * No libc, no SSE (built with -mgeneral-regs-only), no stack protector.
 * ===========================================================================
 */
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

#define COM1 0x3F8

static void serial_putc(char c) {
    /* poll Line Status Register until the transmit holding register is empty */
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s) {
    for (; *s; ++s)
        serial_putc(*s);
}

/* Write a string to one row of the 80x25 VGA text buffer, white-on-black. */
static void vga_puts_line(const char *s, int row) {
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    vga += (uint64_t)row * 80;
    for (int i = 0; s[i]; ++i)
        vga[i] = (uint16_t)((uint8_t)s[i]) | (uint16_t)(0x0F << 8);
}

void kmain(void) {
    serial_puts("NEXUS: entered kmain (64-bit long mode, ring 0)\r\n");
    vga_puts_line("NEXUS KERNEL OK", 1);
    serial_puts("NEXUS KERNEL OK\r\n");

    for (;;)
        __asm__ volatile("hlt");
}
