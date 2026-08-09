/* kernel/mm/numa.h — ACPI SRAT topology (Group 3 item 17, DDR-882).
 *
 * Parsed by the KERNEL after acpi_init(), not handed over by the bootloader —
 * see the note above struct numa_topology in boot_info.h for why.
 */
#pragma once
#include <stdint.h>
#include "boot_info.h"

/* Parse SRAT. Safe to call once, after acpi_init(). A machine with no SRAT is
 * not an error: topology.valid stays 0 and every query reports node 0. */
void numa_init(void);

const struct numa_topology *numa_get(void);

/* Node owning a physical address, or 0 when topology is unknown. Never fails —
 * a caller in the allocator's free path has no way to handle an error. */
uint32_t numa_node_of(uint64_t phys);

/* Node of a CPU by APIC id, or 0 when unknown. Item 37 consumes this. */
uint32_t numa_node_of_cpu(uint32_t apic_id);

uint32_t numa_node_count(void);
