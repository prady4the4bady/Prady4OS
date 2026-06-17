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
#include "boot_info.h"
#include "irq.h"

extern void gdt_init(void);    /* arch/x86_64/cpu.asm */
extern void idt_init(void);    /* kernel/idt.c        */

static void print_boot_info(const struct boot_info *bi) {
    if (bi->magic != BOOT_INFO_MAGIC) {
        kputs("NEXUS: WARNING bad boot_info magic=");
        kputhex(bi->magic);
        kputs("\r\n");
        return;
    }
    kputs("NEXUS: boot_info OK  vendor=");
    kputs(bi->cpu_vendor);
    kputs("  long_mode=");
    kputhex(bi->long_mode);
    kputs("\r\n");
    kputs("NEXUS: E820 map, entries=");
    kputhex(bi->e820_count);
    kputs("\r\n");
    for (uint32_t i = 0; i < bi->e820_count; i++) {
        kputs("  base=");
        kputhex(bi->e820[i].base);
        kputs(" len=");
        kputhex(bi->e820[i].len);
        kputs(" type=");
        kputhex(bi->e820[i].type);
        kputs("\r\n");
    }
}

void kmain(struct boot_info *bi) {
    kputs("NEXUS: entered kmain (64-bit long mode, ring 0)\r\n");

    print_boot_info(bi);

    gdt_init();
    kputs("NEXUS: kernel GDT loaded\r\n");

    idt_init();
    kputs("NEXUS: IDT loaded (48 vectors: 32 exceptions + 16 IRQ)\r\n");

    kvga_line("NEXUS KERNEL OK", 1);
    kputs("NEXUS KERNEL OK\r\n");

    /* Self-test: a breakpoint must be caught by the IDT and resume execution. */
    kputs("NEXUS: IDT self-test — executing int3...\r\n");
    __asm__ volatile("int3");
    kputs("NEXUS: resumed after int3 — exception handling verified\r\n");

    /* Hardware interrupts: PIC + PIT, then enable and watch the clock tick. */
    pic_remap();
    pit_init(100);                       /* 100 Hz */
    kputs("NEXUS: PIC remapped, PIT @100Hz; enabling interrupts (sti)\r\n");
    __asm__ volatile("sti");

    while (g_ticks < 5)                   /* prove IRQ0 actually fires */
        __asm__ volatile("hlt");
    kputs("NEXUS: timer IRQ alive, ticks=");
    kputhex(g_ticks);
    kputs("\r\n");

    kputs("NEXUS: idle (halt, interrupts on)\r\n");
    for (;;)
        __asm__ volatile("hlt");
}
