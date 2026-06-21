/* kernel/mm/vmm_cow.h — copy-on-write fork (IMP-D, replaces copy-all-pages).
 *
 * fork shares the parent's user pages with the child instead of copying them:
 * every shared frame is reference-counted (pmm_incref) and writable pages are
 * marked read-only + PTE_SW_COW in BOTH parent and child. The first write to
 * such a page traps to vmm_cow_fault, which gives the writer a private copy (or
 * simply re-grants write access if it is already the sole owner).
 */
#pragma once
#include <stdint.h>

/* Build a COW child of `parent_cr3`: shares + refcounts user frames, marking
 * writable ones RO+COW in both spaces. Returns the new CR3, or 0 on OOM. */
uint64_t vmm_fork_address_space_cow(uint64_t parent_cr3);

/* #PF handler hook for a user write (error code 0x7) at `fault_va` in `cr3`.
 * Returns 0 if it was a COW page and the fault was resolved (resume the
 * instruction), or -1 if `fault_va` is not a COW page (a real fault — caller
 * kills the process, preserving W^X enforcement). */
int vmm_cow_fault(uint64_t cr3, uint64_t fault_va);
