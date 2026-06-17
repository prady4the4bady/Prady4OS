/* kernel/console.h — minimal ring-0 console (COM1 serial + VGA text). */
#pragma once
#include <stdint.h>

void kputc(char c);                       /* one byte to COM1 */
void kputs(const char *s);                /* NUL-terminated string to COM1 */
void kputhex(uint64_t v);                 /* "0x" + 16 hex digits to COM1 */
void kputdec(uint64_t v);                 /* unsigned decimal to COM1 */
void kvga_line(const char *s, int row);   /* write a string to one VGA text row */
