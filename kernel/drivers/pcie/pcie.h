/* kernel/drivers/pcie/pcie.h — PCIe enumeration via MMCONFIG/ECAM (Phase 3).
 *
 * Reads the ACPI MCFG table for the ECAM base, maps it, and scans the bus for
 * devices. Config space is addressed as base + (bus<<20 | dev<<15 | func<<12).
 * Requires a PCIe machine with an MCFG table (QEMU q35).
 */
#pragma once
#include <stdint.h>

struct pcie_device {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
};

void     pcie_init(void);
uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off);
void     pcie_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t val);
unsigned pcie_device_count(void);
const struct pcie_device *pcie_device_get(unsigned i);

/* MSI-X capability helpers (DDR-774a) — device-agnostic, so any driver (virtio
 * today, NVMe in DDR-774b) can program its own vector. The caller maps the table
 * BAR itself and passes the VA of entry 0. */

/* Locate the MSI-X capability (ID 0x11). Returns its config-space offset, or 0
 * when absent. On success *bir is the table's BAR index and *table_off its byte
 * offset within that BAR. */
uint8_t pcie_msix_find(uint8_t bus, uint8_t dev, uint8_t func,
                       uint8_t *bir, uint32_t *table_off);

/* Enable MSI-X and program table entry 0 to deliver `vector` to `apic_id`
 * (fixed delivery, edge, unmasked). `entry0` is the mapped VA of the table. */
void pcie_msix_program(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap,
                       volatile uint32_t *entry0, uint8_t vector, uint32_t apic_id);

/* Disable this function's INTx assertions (command register bit 10). */
void pcie_intx_disable(uint8_t bus, uint8_t dev, uint8_t func);
