/* kernel/boot_info.h
 * The boot -> kernel handoff ABI. The bootloader (boot/stage2/stage2.asm) fills
 * this struct at a fixed physical address and passes its pointer to kmain in RDI.
 * Keep this layout byte-for-byte in sync with the BOOT_INFO offsets in stage2.
 */
#pragma once
#include <stdint.h>

#define BOOT_INFO_MAGIC 0x59445250u   /* 'PRDY' */

/* One BIOS INT 15h E820 entry (we always request 24-byte entries). */
struct e820_entry {
    uint64_t base;
    uint64_t len;
    uint32_t type;     /* 1 = usable, 2 = reserved, 3 = ACPI reclaim, ... */
    uint32_t acpi;     /* ACPI 3.0 extended attributes */
} __attribute__((packed));

/* ---- NUMA topology (DDR-882, item 17) ---------------------------------
 *
 * This struct is DECLARED here because boot_info.h is where machine topology
 * lives, but it is NOT part of the boot handoff and the bootloader never writes
 * it. Two reasons, both structural:
 *
 *   1. `struct boot_info` ends in a flexible array (`e820[]`). Nothing can be
 *      appended after it — a trailing flexible array must be last, and a block
 *      placed there would overlap the E820 entries.
 *   2. Topology comes from ACPI SRAT, which needs RSDP discovery, checksum
 *      validation and sub-table walking. stage2.asm is 1294 bytes of 16-bit
 *      assembly; the kernel reads SRAT itself in numa_init().
 *
 * The boot protocol is therefore unchanged by this feature.
 */
#define NUMA_MAX_NODES  8
#define NUMA_MAX_RANGES 32
#define NUMA_MAX_CPUS   64

struct numa_range {
    uint64_t base;             /* page-aligned; unaligned ranges are REJECTED */
    uint64_t len;
    uint32_t node;
    uint32_t _pad;
};

struct numa_topology {
    uint32_t valid;            /* 0 = no SRAT; treat the machine as one node   */
    uint32_t node_count;
    uint32_t range_count;
    uint32_t rejected;         /* sub-tables refused, not absorbed (DDR-882 §4) */
    struct numa_range range[NUMA_MAX_RANGES];
    uint8_t  cpu_node[NUMA_MAX_CPUS];   /* APIC id -> node; 0xFF = unknown     */
};

struct boot_info {
    uint32_t magic;            /* BOOT_INFO_MAGIC */
    uint32_t e820_count;       /* number of valid e820[] entries */
    char     cpu_vendor[16];   /* CPUID leaf 0 vendor string, NUL-terminated */
    uint32_t long_mode;        /* 1 if CPUID 80000001h EDX.29 set */
    uint32_t reserved;
    struct e820_entry e820[];  /* e820_count entries follow the header */
} __attribute__((packed));
