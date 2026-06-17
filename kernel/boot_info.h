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

struct boot_info {
    uint32_t magic;            /* BOOT_INFO_MAGIC */
    uint32_t e820_count;       /* number of valid e820[] entries */
    char     cpu_vendor[16];   /* CPUID leaf 0 vendor string, NUL-terminated */
    uint32_t long_mode;        /* 1 if CPUID 80000001h EDX.29 set */
    uint32_t reserved;
    struct e820_entry e820[];  /* e820_count entries follow the header */
} __attribute__((packed));
