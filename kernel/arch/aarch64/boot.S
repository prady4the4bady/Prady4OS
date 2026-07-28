/* kernel/arch/aarch64/boot.S — aarch64 entry (ADR-034).
 *
 * QEMU `-machine virt -kernel <ELF>` honours the ELF entry point and enters
 * with the MMU off. Everything here is the minimum to reach C safely:
 * park every core but the primary, install a stack, jump.
 *
 * GAS syntax through clang's integrated assembler — nasm is x86-only (ADR-034).
 */

    .section .text.boot, "ax"
    .globl  _start
    .type   _start, %function
_start:
    /* MPIDR_EL1[7:0] is Aff0 — the core number within the cluster. Park every
     * core except 0. Without this, all four cores run the same C entry and
     * interleave their console writes, which looks like a corrupted sentinel
     * rather than the missing SMP bring-up it actually is. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, .Lpark

    /* Stack grows down from the top of the reserved region. The linker script
     * aligns __stack_top to 16, which the AArch64 PCS requires of SP. */
    adrp    x0, __stack_top
    add     x0, x0, :lo12:__stack_top
    mov     sp, x0

    /* Zero BSS before C runs — .bss is NOBITS, so nothing loaded it and any
     * static in start.c would otherwise read whatever the firmware left. */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
.Lbss:
    cmp     x1, x2
    b.hs    .Lbss_done
    str     xzr, [x1], #8
    b       .Lbss
.Lbss_done:

    bl      arch_main

.Lpark:
    /* wfe rather than a spin: a parked core must not burn a CI runner's CPU
     * for the whole boot window. */
    wfe
    b       .Lpark
    .size   _start, . - _start
