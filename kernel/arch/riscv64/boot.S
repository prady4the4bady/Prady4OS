/* kernel/arch/riscv64/boot.S — riscv64 entry (ADR-034).
 *
 * OpenSBI (`-bios default`) hands off at 0x8020_0000 in S-mode with
 *   a0 = hartid, a1 = DTB pointer.
 * a0 is read BEFORE the stack is set up, because the parking decision has to be
 * made while the register still holds what firmware put there.
 */

    .section .text.boot, "ax"
    .globl  _start
    .type   _start, @function
_start:
    /* Park every hart but 0. OpenSBI may bring up several; without this they
     * share one stack and one console, which corrupts the sentinel rather than
     * failing cleanly. */
    bnez    a0, .Lpark

    la      sp, __stack_top

    /* Zero BSS — .bss is NOBITS, so nothing loaded it. */
    la      t0, __bss_start
    la      t1, __bss_end
.Lbss:
    bgeu    t0, t1, .Lbss_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .Lbss
.Lbss_done:

    call    arch_main

.Lpark:
    wfi
    j       .Lpark
    .size   _start, . - _start
