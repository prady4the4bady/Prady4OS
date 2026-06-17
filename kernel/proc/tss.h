/* kernel/proc/tss.h — 64-bit Task State Segment (Phase 2e).
 *
 * In long mode the TSS mainly supplies RSP0: the stack the CPU switches to when
 * an interrupt or exception is taken while running in ring 3. tss_set_rsp0 must
 * point at the current user thread's kernel stack before it runs in ring 3.
 */
#pragma once
#include <stdint.h>

void tss_init(uint64_t rsp0);        /* fill TSS, patch GDT descriptor, LTR */
void tss_set_rsp0(uint64_t rsp0);    /* update the ring-0 stack for ring-3 entry */
