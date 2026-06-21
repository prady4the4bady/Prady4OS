/* kernel/mm/vmm_fork.h — address-space duplication for fork (Phase 5b slice 8).
 *
 * Copy-all-pages fork (the ADR-022 baseline; copy-on-write is the IMP-D
 * follow-on). Builds a brand-new address space that is a deep copy of every
 * present user page in the parent — kernel mappings are shared, not copied.
 */
#pragma once
#include <stdint.h>

/* Deep-copy the user portion of `parent_cr3` into a fresh address space.
 * Returns the new CR3 (page-aligned physical addr), or 0 on OOM (any partial
 * child is torn down before returning). */
uint64_t vmm_fork_address_space_copy(uint64_t parent_cr3);
