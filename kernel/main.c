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
#include "pmm.h"
#include "kheap.h"
#include "vmm.h"

extern void gdt_init(void);    /* arch/x86_64/cpu.asm */
extern void idt_init(void);    /* kernel/idt.c        */

static void vmm_test(void) {
    const uint64_t va = 0xFFFF800000000000ull;   /* unused PML4 slot (256) */
    uint64_t pg = pmm_alloc_page();
    if (!pg) {
        kputs("NEXUS: vmm test — no frame\r\n");
        return;
    }
    uint64_t before = kheap_outstanding();
    if (vmm_map(va, pg, VMM_RW) != 0) {
        kputs("NEXUS: vmm_map FAILED\r\n");
        pmm_free_page(pg);
        return;
    }
    volatile uint64_t *p = (volatile uint64_t *)va;
    p[0] = 0xCAFEBABEDEADBEEFull;
    uint64_t rb = p[0];

    kputs("NEXUS: vmm_map va=");
    kputhex(va);
    kputs(" pa=");
    kputhex(pg);
    kputs(" readback=");
    kputhex(rb);
    kputs(rb == 0xCAFEBABEDEADBEEFull ? "  (OK)\r\n" : "  (FAIL)\r\n");

    vmm_unmap(va);
    pmm_free_page(pg);
    uint64_t after = kheap_outstanding();
    kputs("NEXUS: vmm unmap reclaim — outstanding ");
    kputhex(before);
    kputs(" -> ");
    kputhex(after);
    kputs(after == before ? "  (clean)\r\n" : "  (LEAK!)\r\n");
}

static void kheap_stress(void) {
    kheap_init();
    uint64_t base = kheap_outstanding();

    /* Mixed-size churn: some land in slab caches, some are whole-page large. */
    void *p[64];
    for (int i = 0; i < 64; i++) {
        size_t sz = (size_t)(((unsigned)i * 37u + 8u) & 0xFFFu) + 1u;  /* 1..4096 */
        p[i] = kmalloc(sz);
        if (p[i]) {
            ((volatile unsigned char *)p[i])[0] = (unsigned char)i;
            ((volatile unsigned char *)p[i])[sz - 1] = (unsigned char)~i;
        }
    }
    for (int i = 0; i < 64; i++)
        kfree(p[i]);

    /* Dedicated object pools. */
    void *a = pcb_alloc(), *b = cap_alloc(), *c = ipc_alloc(), *d = ptnode_alloc();
    pcb_free(a); cap_free(b); ipc_free(c); ptnode_free(d);

    uint64_t after = kheap_outstanding();
    kputs("NEXUS: kheap stress — outstanding base=");
    kputhex(base);
    kputs(" after=");
    kputhex(after);
    kputs(after == base ? "  (no leak)\r\n" : "  (LEAK!)\r\n");
}

static void pmm_selftest(const struct boot_info *bi) {
    pmm_init(bi);
    kputs("NEXUS: PMM (buddy) free frames=");
    kputhex(pmm_free_page_count());
    kputs("\r\n");

    uint64_t start = pmm_free_page_count();
    uint64_t a = pmm_alloc_page();        /* order 0 */
    uint64_t b = pmm_alloc_pages(3);      /* order 3 = 8 frames */
    kputs("  alloc 1 frame -> ");
    kputhex(a);
    kputs("\r\n  alloc 8 frames -> ");
    kputhex(b);
    kputs("\r\n  free frames after alloc=");
    kputhex(pmm_free_page_count());
    kputs("\r\n");

    pmm_free_pages(b, 3);
    pmm_free_page(a);
    uint64_t end = pmm_free_page_count();
    kputs("  free frames after release=");
    kputhex(end);
    kputs(end == start ? "  (balanced)\r\n" : "  (LEAK!)\r\n");
}

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

    pmm_selftest(bi);
    kheap_stress();
    vmm_test();

    kputs("NEXUS: idle (halt, interrupts on)\r\n");
    for (;;)
        __asm__ volatile("hlt");
}
