/* kernel/fault_expect.h — DDR-1040: the one-shot expected-fault latch.
 *
 * A ring-0 exception in this kernel is fatal: idt.c has no fixup table, so the
 * CPL-0 path falls through to the panic. That makes any HARDWARE ENFORCEMENT
 * unobservable — a probe that proves SMEP (or, in DDR-1041, SMAP) is actually
 * refusing an access has to cause the violation and survive it.
 *
 * This latch is the narrowest thing that allows that, and every one of its
 * limits is deliberate (DDR-1040 §4):
 *
 *   - ONE SHOT. Armed explicitly; disarmed by the first matching fault. A latch
 *     left armed would silence every later fault — the failure DDR-1019 found in
 *     the panic arbitration, where a winner that could not print silenced every
 *     subsequent panic.
 *   - RIP-WINDOWED. Only a fault from the instruction that was meant to fault is
 *     caught; a fault anywhere else panics exactly as before.
 *   - SINGLE-CPU BY PRECONDITION, enforced not commented: fault_expect_arm()
 *     REFUSES (and says so) unless interrupts are masked and no AP is online.
 *     The only caller runs beside uaccess_selftest (main.c), well before
 *     smp_start_aps(). A per-CPU latch would need the GS-independent
 *     percpu_by_apic_id(lapic_id()) form DDR-1010 had to use, and nothing wants
 *     one.
 *   - IN BSS. Statics, zero by definition, so §NON-NEGOTIABLE 10's
 *     kmalloc-does-not-zero trap does not arise here.
 *
 * This is NOT __ex_table. copyin/copyout still VALIDATE rather than
 * fault-and-recover, and that is not changed by this file existing.
 */
#pragma once
#include <stdint.h>

/* Arm the latch: a CPL-0 fault whose RIP is in [lo, hi) resumes at `resume`.
 * Returns 1 if armed, 0 if refused (an AP is online, IF is set, or the window
 * is empty) — a refusal prints its reason and leaves the latch disarmed. */
int fault_expect_arm(uint64_t lo, uint64_t hi, uint64_t resume);

/* Disarm. Returns 1 if the latch fired, and then stores the faulting vector and
 * error code through the out-pointers (either may be NULL). */
int fault_expect_taken(uint32_t *vec_out, uint32_t *err_out);
