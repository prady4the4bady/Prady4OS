/* kernel/drivers/virtio/virtio_pci.h — modern (1.0) virtio-over-PCIe transport. */
#pragma once
#include <stdint.h>
#include "virtio.h"

struct virtio_pci_dev {
    uint8_t  bus, dev, func;
    uint8_t  irq;                          /* INTx line (PCI config 0x3C) */
    volatile uint8_t *common;              /* common config structure   */
    volatile uint8_t *notify;              /* notify region base         */
    volatile uint8_t *isr;                 /* ISR status byte            */
    volatile uint8_t *devcfg;              /* device-specific config     */
    uint32_t notify_mult;                  /* notify_off_multiplier      */
};

/* Locate the virtio PCI capabilities, map their BARs. 0 on success. */
int      virtio_pci_attach(struct virtio_pci_dev *d, uint8_t bus, uint8_t dev, uint8_t func);
/* RESET→ACK→DRIVER→negotiate(features & wanted)→FEATURES_OK. Returns the
 * accepted feature bits, or 0 on failure (FEATURES_OK rejected). */
uint64_t virtio_pci_negotiate(struct virtio_pci_dev *d, uint64_t wanted);
/* Allocate + program virtqueue `qidx`. 0 on success. */
int      virtio_pci_setup_queue(struct virtio_pci_dev *d, struct virtq *vq, uint16_t qidx);
void     virtio_pci_driver_ok(struct virtio_pci_dev *d);
void     virtio_pci_notify(struct virtio_pci_dev *d, struct virtq *vq, uint16_t qidx);
uint8_t  virtio_pci_isr_ack(struct virtio_pci_dev *d);   /* read ISR (read-to-clear) */
