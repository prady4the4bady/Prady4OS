/* kernel/drivers/input/ps2kbd.h — PS/2 keyboard: ASCII ring + key events.
 *
 * DDR-703 shipped the ASCII ring. DDR-991 adds extended-scancode decode,
 * modifier tracking and a structured event ring alongside it — the byte stream
 * below is UNCHANGED and its two ring-3 consumers (inputtest, compositor) are
 * unaffected. A chord is not a character, which is why it needed its own path.
 */
#pragma once
#include <stdint.h>

/* ---- DDR-991: modifier bitmask -------------------------------------------
 * Sampled AT THE EVENT, never read afterwards: a consumer that polled later
 * would see the modifier already released, which is the classic chord race. */
#define KMOD_SHIFT    0x01u
#define KMOD_CTRL     0x02u
#define KMOD_ALT      0x04u
#define KMOD_META     0x08u   /* Super / Windows */
#define KMOD_SHIFT_R  0x10u   /* the L/R distinction, for anyone who wants it; */
#define KMOD_CTRL_R   0x20u   /* KMOD_CTRL is set for either side, so a chord  */
#define KMOD_ALT_R    0x40u   /* test never has to care which one is held.     */
#define KMOD_META_R   0x80u

/* ---- DDR-991: normalised keycodes ----------------------------------------
 * `code` is NOT a raw scancode — ring 3 never sees scancode-set trivia. Keys
 * with a printable form report that character (so KEY 'c' is 'c'); everything
 * else takes a value at 0x80+, clear of ASCII. */
#define KEY_ESC       0x1Bu
#define KEY_TAB       0x09u
#define KEY_ENTER     0x0Au
#define KEY_BACKSPACE 0x08u

#define KEY_F1        0x80u   /* F1..F12 are contiguous: KEY_F1 + (n-1) */
#define KEY_F12       0x8Bu
#define KEY_UP        0x90u
#define KEY_DOWN      0x91u
#define KEY_LEFT      0x92u
#define KEY_RIGHT     0x93u
#define KEY_HOME      0x94u
#define KEY_END       0x95u
#define KEY_PGUP      0x96u
#define KEY_PGDN      0x97u
#define KEY_INSERT    0x98u
#define KEY_DELETE    0x99u
#define KEY_SHIFT     0xA0u   /* the modifier keys themselves, so a consumer   */
#define KEY_CTRL      0xA1u   /* can see the press/release edge if it wants it */
#define KEY_ALT       0xA2u
#define KEY_META      0xA3u

/* 4 bytes, fixed — this is a ring-3 ABI (SYS_KEY_POLL, NSI 96). */
struct key_ev {
    uint8_t code;    /* KEY_* / printable char */
    uint8_t mods;    /* KMOD_* bitmask at the moment of this event */
    uint8_t down;    /* 1 = make, 0 = break */
    uint8_t ascii;   /* printable equivalent, or 0 — a convenience, not identity */
};

void ps2kbd_isr(void);                 /* IRQ1 handler body: read 0x60, decode */
int  ps2kbd_pop(char *buf, int max);   /* DDR-703 ASCII stream — UNCHANGED */
int  ps2kbd_pop_ev(struct key_ev *buf, int max);   /* DDR-991 structured events */
