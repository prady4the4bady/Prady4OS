/* kernel/drivers/ahci/ahci.h — AHCI (SATA) block driver (Group 4 item 23,
 * DDR-875).
 *
 * Probed from the PCI scan for class 0x01 subclass 0x06 (SATA) with prog-if
 * 0x01 (AHCI). Each port carrying a SATA disk registers as a blk_device, so
 * SFS/FAT/ext4 mount on it exactly as they do on virtio-blk.
 */
#pragma once
#include <stdint.h>

void ahci_init(uint8_t bus, uint8_t dev, uint8_t func);

/* Diagnostics for the gate: ports found with a device attached. */
unsigned ahci_port_count(void);
