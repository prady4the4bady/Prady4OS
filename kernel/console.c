/* kernel/console.c
 * Minimal ring-0 console for early bring-up: COM1 serial (the smoke test reads
 * this) and the VGA text buffer (for a graphical boot). No locking yet — there
 * is exactly one CPU running with interrupts off at this stage.
 */
#include "console.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

#define COM1 0x3F8

void kputc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }   /* wait for THRE */
    outb(COM1, (uint8_t)c);
}

void kputs(const char *s) {
    for (; *s; ++s)
        kputc(*s);
}

void kputhex(uint64_t v) {
    static const char digits[] = "0123456789ABCDEF";
    kputc('0');
    kputc('x');
    for (int shift = 60; shift >= 0; shift -= 4)
        kputc(digits[(v >> shift) & 0xF]);
}

void kvga_line(const char *s, int row) {
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    vga += (uint64_t)row * 80;
    for (int i = 0; s[i]; ++i)
        vga[i] = (uint16_t)((uint8_t)s[i]) | (uint16_t)(0x0F << 8);
}
