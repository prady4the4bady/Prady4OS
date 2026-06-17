/* kernel/main.c
 * ===========================================================================
 * NEXUS kernel — long-mode entry point (Phase 2a).
 *
 * Reached in 64-bit long mode, ring 0, on a flat identity map of the low 1 GiB
 * set up by the bootloader. This slice installs the kernel's own GDT and an IDT
 * with handlers for all 32 CPU exceptions, then runs a recoverable #BP self-test
 * to prove the IDT works. Still no allocator, no scheduler, no hardware
 * interrupts (no PIC/APIC) — those are later slices.
 *
 * Built -ffreestanding -mgeneral-regs-only (no SSE), -fno-omit-frame-pointer.
 * ===========================================================================
 */
#include "console.h"

extern void gdt_init(void);    /* arch/x86_64/cpu.asm */
extern void idt_init(void);    /* kernel/idt.c        */

void kmain(void) {
    kputs("NEXUS: entered kmain (64-bit long mode, ring 0)\r\n");

    gdt_init();
    kputs("NEXUS: kernel GDT loaded\r\n");

    idt_init();
    kputs("NEXUS: IDT loaded (32 exception vectors)\r\n");

    kvga_line("NEXUS KERNEL OK", 1);
    kputs("NEXUS KERNEL OK\r\n");

    /* Self-test: a breakpoint must be caught by the IDT and resume execution. */
    kputs("NEXUS: IDT self-test — executing int3...\r\n");
    __asm__ volatile("int3");
    kputs("NEXUS: resumed after int3 — exception handling verified\r\n");

    kputs("NEXUS: idle (halt)\r\n");
    for (;;)
        __asm__ volatile("hlt");
}
