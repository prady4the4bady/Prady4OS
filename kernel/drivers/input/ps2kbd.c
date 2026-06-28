/* kernel/drivers/input/ps2kbd.c — PS/2 keyboard scancode->ASCII ring (DDR-703).
 *
 * The IRQ1 handler reads a scancode-set-1 byte from port 0x60, tracks Shift, and
 * pushes the translated ASCII into a small ring that SYS_INPUT_POLL drains. Does
 * the minimum in IRQ context (no allocation), like the console-RX handler.
 */
#include "ps2kbd.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Scancode set 1, make codes 0x00..0x39 -> ASCII (unshifted / shifted columns).
 * 0 = no printable mapping. Covers the main alphanumeric block + space/enter/tab. */
static const char map_lower[0x40] = {
    0,   27,  '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,  'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0,  '*', 0,  ' ', 0,  0,   0,  0,  0,  0,
};
static const char map_upper[0x40] = {
    0,   27,  '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,  'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0,  '|','Z','X','C','V',
    'B','N','M','<','>','?', 0,  '*', 0,  ' ', 0,  0,   0,  0,  0,  0,
};

#define KBD_RING 256u
static volatile char     g_ring[KBD_RING];
static volatile uint16_t g_head, g_tail;     /* head: producer (IRQ); tail: consumer */
static int g_shift;

void ps2kbd_isr(void) {
    uint8_t sc = inb(0x60);
    if (sc == 0x2A || sc == 0x36) { g_shift = 1; return; }   /* Shift make */
    if (sc == 0xAA || sc == 0xB6) { g_shift = 0; return; }   /* Shift break */
    if (sc & 0x80)                                            /* any other break code */
        return;
    if (sc >= 0x40)
        return;
    char c = g_shift ? map_upper[sc] : map_lower[sc];
    if (!c)
        return;
    uint16_t nh = (uint16_t)((g_head + 1) & (KBD_RING - 1));
    if (nh != g_tail) {                                       /* drop on full */
        g_ring[g_head] = c;
        g_head = nh;
    }
}

int ps2kbd_pop(char *buf, int max) {
    int n = 0;
    while (n < max && g_tail != g_head) {
        buf[n++] = g_ring[g_tail];
        g_tail = (uint16_t)((g_tail + 1) & (KBD_RING - 1));
    }
    return n;
}
