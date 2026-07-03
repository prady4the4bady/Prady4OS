/* kernel/apic/smp.h — SMP bring-up, APs parked (DDR-714 stage B, ADR-029). */
#pragma once
#include <stdint.h>

/* Boot every MADT-listed AP via INIT-SIPI-SIPI into a parked hlt loop, one at
 * a time (the 0x8000 trampoline mailbox is shared). Prints the final
 * "[smp] cpus online=N/M". Requires the LAPIC up + the PIT/APIC tick alive
 * (delays ride g_ticks). Safe no-op when only one CPU is listed. */
void smp_start_aps(void);

/* AP work dispatch (DDR-SMP-3c-alpha): post a kernel function to an idle AP's
 * single-slot mailbox and wake it via IPI; poll completion with smp_job_done. */
int smp_run_on(uint32_t cpu_idx, void (*fn)(void));
int smp_job_done(uint32_t cpu_idx);
