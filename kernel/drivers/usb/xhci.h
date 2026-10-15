/* kernel/drivers/usb/xhci.h — xHCI controller bring-up (DDR-1161, USB piece 1). */
#ifndef PRADYOS_XHCI_H
#define PRADYOS_XHCI_H
#include <stdint.h>
void xhci_init(uint8_t bus, uint8_t dev, uint8_t func);
void xhci_poll(void);   /* DDR-1167: BSP timer tick only */
#endif
