/* kernel/drivers/net/e1000e.h — Intel e1000e NIC (Group 4 item 25, DDR-876).
 *
 * Same shape as virtio_net.h on purpose: whichever NIC the platform has, the
 * lwIP bridge above sees one interface. A second, differently-shaped API would
 * push the "which driver am I talking to" question up into the stack.
 */
#pragma once
#include <stdint.h>

void e1000e_init(uint8_t bus, uint8_t dev, uint8_t func);

int  e1000e_tx(const void *frame, uint32_t len);            /* 0 on success  */
void e1000e_set_rx(void (*cb)(const uint8_t *frame, uint32_t len));
void e1000e_mac(uint8_t out[6]);
int  e1000e_up(void);                                       /* 1 once ready  */

/* Driven from the timer path: this driver is polled, not interrupt-driven.
 * See the .c header for why that is a deliberate choice and not a shortcut. */
void e1000e_poll(void);
