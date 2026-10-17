/* kernel/mm/uaccess.h — validated user<->kernel memory copy (Phase 5b, ADR-022).
 * ===========================================================================
 * Every byte that crosses the ring boundary goes through these three primitives.
 * The kernel NEVER dereferences a raw user pointer anywhere else — a bare
 * `*user_ptr` / memcpy from a user address in a syscall handler is a defect.
 *
 * The contract (binding, equal in standing to W^X — see ADR-022):
 *   - the user range is validated against the calling process's page tables
 *     BEFORE any dereference, so the kernel never takes a #PF at CPL 0 on a
 *     user-supplied address;
 *   - a bad pointer returns -EFAULT (the kernel keeps running), never a panic;
 *   - copyout additionally requires the target pages to be user-WRITABLE, so a
 *     write to a read-only / text user page is -EFAULT, never an honored write
 *     (this is also how W^X is upheld on the copy path).
 *
 * User pointers are tagged `__user` purely as a grep marker (expands to nothing).
 * ===========================================================================
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef __user
#define __user            /* marker: this pointer is an untrusted user VA */
#endif

typedef long ssize_t;

/* ---- DDR-1041: SMAP ------------------------------------------------------ *
 * CR4.SMAP makes a ring-0 DATA access through a user translation fault unless
 * EFLAGS.AC is set. That turns the paragraph above from a documented contract
 * into a hardware-enforced one: with SMAP on, a bare *user_ptr anywhere outside
 * these three primitives is not a defect someone has to notice in review, it is
 * a fault that names its own RIP.
 *
 * WHY THIS IS A RUNTIME BRANCH AND NOT AN UNCONDITIONAL stac. `stac`/`clac`
 * are #UD on a CPU without SMAP, and the TCG default (qemu64) does not have it
 * — measured, DDR-1040 §2 — so an unconditional pair would kill every gate.
 * One predictable branch per copy is the price; the alternative is
 * alternatives-patching, which is a great deal of machinery for one call site.
 *
 * WHAT THIS DOES NOT COVER, stated rather than implied: an interrupt taken
 * between begin() and end() runs its handler with AC still set, because the CPU
 * clears IF on an interrupt gate but does NOT clear AC. Inside that window SMAP
 * is effectively off for the interrupted CPU. Linux clears AC on kernel entry
 * for exactly this reason; this kernel does not, and DDR-1041 §8 records it as a
 * measured residual rather than a fixed one. */
extern volatile unsigned g_smap_on;      /* set by cpu_enable_smap (smp.c) */

static inline void uaccess_begin(void) {
    if (g_smap_on) __asm__ volatile("stac" ::: "cc");
}
static inline void uaccess_end(void) {
    if (g_smap_on) __asm__ volatile("clac" ::: "cc");
}

/* Copy `n` bytes user->kernel. Returns n on success, -EFAULT on a bad source. */
ssize_t copyin(void *kdst, const void __user *usrc, size_t n);

/* Copy `n` bytes kernel->user. Returns n on success, -EFAULT if the destination
 * is not present+user+writable for the whole range. */
ssize_t copyout(void __user *udst, const void *ksrc, size_t n);

/* Copy a NUL-terminated string user->kernel, at most `max` bytes incl. the NUL.
 * On success returns the string length (excluding NUL) and, if `lenout` != NULL,
 * stores that length; returns -EFAULT on a bad page, -ENAMETOOLONG if no NUL is
 * found within `max` bytes. */
ssize_t copyinstr(char *kdst, const void __user *usrc, size_t max, size_t *lenout);
