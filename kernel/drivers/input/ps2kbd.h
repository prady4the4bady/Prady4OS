/* kernel/drivers/input/ps2kbd.h — PS/2 keyboard -> ASCII ring (Layer 7, DDR-703). */
#pragma once
#include <stdint.h>

void ps2kbd_isr(void);                 /* IRQ1 handler body: read 0x60, push ASCII */
int  ps2kbd_pop(char *buf, int max);   /* drain up to max buffered chars; returns count */
