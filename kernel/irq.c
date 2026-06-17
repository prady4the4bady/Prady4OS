/* kernel/irq.c — 8259 PIC remap/EOI and 8254 PIT setup (Phase 2a). */
#include "irq.h"
#include "io.h"

volatile uint64_t g_ticks = 0;

#define PIC1        0x20    /* master command */
#define PIC1_DATA   0x21
#define PIC2        0xA0    /* slave command */
#define PIC2_DATA   0xA1
#define PIC_EOI     0x20

#define PIT_CH0     0x40
#define PIT_CMD     0x43
#define PIT_HZ      1193182u

void pic_remap(void) {
    /* ICW1: begin init, expect ICW4 */
    outb(PIC1, 0x11); io_wait();
    outb(PIC2, 0x11); io_wait();
    /* ICW2: vector offsets — master 0x20, slave 0x28 */
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    /* ICW3: master has a slave on IRQ2; slave cascade identity 2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    /* Masks: unmask IRQ0 (timer) + IRQ1 (keyboard) on master, mask all slave. */
    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint64_t vector) {
    if (vector >= 0x28)
        outb(PIC2, PIC_EOI);
    outb(PIC1, PIC_EOI);
}

void pit_init(uint32_t hz) {
    uint32_t divisor = PIT_HZ / hz;
    outb(PIT_CMD, 0x36);                          /* ch0, lo/hi byte, mode 3 */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
}
