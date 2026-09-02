/* kernel/apic/smp.h — SMP bring-up, APs parked (DDR-714 stage B, ADR-029). */
#pragma once
#include <stdint.h>

/* Boot every MADT-listed AP via INIT-SIPI-SIPI into a parked hlt loop, one at
 * a time (the 0x8000 trampoline mailbox is shared). Prints the final
 * "[smp] cpus online=N/M". Requires the LAPIC up + the PIT/APIC tick alive
 * (delays ride g_ticks). Safe no-op when only one CPU is listed. */
void smp_start_aps(void);

/* DDR-1040: CPUs currently running (1 before smp_start_aps). Read by the
 * expected-fault latch, which is single-CPU by precondition. */
unsigned smp_online(void);

/* DDR-1040: set CR4.SMEP on THIS CPU when CPUID reports it. Per-CPU state, so
 * every AP calls it too (smp_ap_entry, beside cpu_enable_sse). Returns a
 * two-bit report: bit 0 = CPUID says the CPU has SMEP, bit 1 = CR4.SMEP is now
 * set. The default QEMU CPU model (qemu64) reports NEITHER — measured, DDR-1040
 * §2 — so 0 is the correct and expected answer on 169 of the 170 gates, and the
 * gate that exercises the feature pins its own -cpu. */
unsigned cpu_enable_smep(void);

/* AP work dispatch (DDR-SMP-3c-alpha): post a kernel function to an idle AP's
 * single-slot mailbox and wake it via IPI; poll completion with smp_job_done. */
int smp_run_on(uint32_t cpu_idx, void (*fn)(void));
int smp_job_done(uint32_t cpu_idx);

/* cap-2b: kick every online AP to reschedule (wake IPI), so it picks up
 * newly-created ready threads from the shared ring. */
void smp_resched_all(void);
/* rq-3: wake ONE idle CPU (directed) so it promptly steals newly-ready work. */
/* DDR-1014: returns 1 if an IPI was actually SENT, 0 if the target could not be
 * kicked (absent, or the BSP -- see smp.c). The caller must not treat a call as
 * a delivered kick: sched_unblock used to `break` after calling this, so a BSP
 * found idle first consumed the one kick and sent nothing. */
int smp_resched_one(uint32_t cpu_idx);
extern volatile uint64_t g_resched_ipis;   /* directed kicks sent (gate proof) */
