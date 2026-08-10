/* kernel/arch/x86_64/pstate.h — CPU frequency scaling (item 27, DDR-892). */
#pragma once
#include <stdint.h>

/* Detect EIST and report. Safe on any CPU: the MSRs are touched only when
 * CPUID.01H:ECX bit 7 says they exist — a blind wrmsr to an unimplemented MSR
 * raises #GP, which at boot is a kernel fault, not a missing feature. */
void pstate_init(void);

int  pstate_available(void);              /* 1 when EIST is present */

/* Current performance state (IA32_PERF_STATUS bits 15:0), or 0 when
 * unavailable. Never invents a value. */
uint16_t pstate_current(void);

/* Request a performance state (IA32_PERF_CTL bits 15:0).
 * Returns 0 on success, -1 when scaling is unavailable. */
int  pstate_set(uint16_t ctl);
