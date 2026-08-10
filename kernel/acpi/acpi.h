/* kernel/acpi/acpi.h — minimal ACPI table discovery (Phase 3).
 *
 * Finds the RSDP, walks the RSDT/XSDT, and returns ACPI tables by signature.
 * This unblocks MCFG (PCIe ECAM, now), and later MADT (APIC) and FADT (power).
 * All table physical addresses are assumed to live in identity-mapped low RAM.
 */
#pragma once
#include <stdint.h>

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

void acpi_init(void);                                       /* locate RSDP + RSDT/XSDT */
const struct acpi_sdt_header *acpi_find_table(const char sig[4]);   /* NULL if absent */

/* DDR-746: FADT (FACP) power management. acpi_power_init parses the PM1x_CNT
 * ports + the DSDT \_S5_ sleep-type values; acpi_poweroff performs an ACPI S5
 * soft-off (does not return on success). acpi_power_available reports whether
 * the S5 path was found (poweroff is a no-op if not). */
void acpi_power_init(void);
int  acpi_power_available(void);
__attribute__((noreturn)) void acpi_poweroff(void);

/* DDR-892 (item 27): S3. Discovery is real; acpi_suspend_s3() deliberately
 * REFUSES until a resume trampoline exists — entering S3 without one does not
 * fail, it never returns. */
int  acpi_s3_available(void);
int  acpi_suspend_s3(void);
__attribute__((noreturn)) void acpi_reboot(void);   /* DDR-747: ACPI/PC reset */
