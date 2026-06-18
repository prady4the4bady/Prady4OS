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
