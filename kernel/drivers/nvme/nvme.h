/* kernel/drivers/nvme/nvme.h — NVMe controller driver (DDR-765).
 *
 * Slice 1 (this file): controller bring-up + Identify over the admin queue.
 * Detection is by PCIe class 0x01 / subclass 0x08 (mass storage / NVM). Block
 * I/O + blk_register land in DDR-766.
 */
#pragma once
#include <stdint.h>

/* Bring up the NVMe controller at the given PCIe B/D/F: map BAR0, enable the
 * controller, create the admin queue, and run Identify Controller + Identify
 * Namespace. No block I/O yet. Safe no-op if the controller never readies. */
void nvme_init(uint8_t bus, uint8_t dev, uint8_t func);
