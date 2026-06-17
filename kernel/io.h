/* kernel/io.h — x86 port I/O primitives shared across kernel modules. */
#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* Short delay by bouncing a byte off an unused port (0x80) — lets slow legacy
 * controllers (PIC/PIT) settle between writes. */
static inline void io_wait(void) {
    outb(0x80, 0);
}
