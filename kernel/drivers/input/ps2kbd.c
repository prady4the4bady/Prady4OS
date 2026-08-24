/* kernel/drivers/input/ps2kbd.c — PS/2 keyboard scancode decode (DDR-703/991).
 *
 * The IRQ1 handler reads a scancode-set-1 byte from port 0x60 and feeds two
 * consumers: the DDR-703 ASCII ring that SYS_INPUT_POLL drains (unchanged), and
 * the DDR-991 structured event ring that SYS_KEY_POLL drains. Does the minimum
 * in IRQ context (no allocation), like the console-RX handler.
 *
 * DDR-991 §1 — WHY 0xE0 AND THE 0x40 CAP HAD TO CHANGE TOGETHER. The old code
 * had no 0xE0 case at all, so an extended key's prefix byte fell through to
 * `if (sc & 0x80) return;` (0xE0 has bit 7 set) and was swallowed as if it were
 * a break code — after which the FOLLOWING byte was decoded as an unprefixed
 * make code. Right-Ctrl (E0 1D) therefore delivered a bare 1D, which in set 1
 * is left-Ctrl's own code. Nothing visibly broke only because every extended
 * key's base byte happened to land either on a modifier or above the
 * `sc >= 0x40` drop. That was luck, and lifting the cap to admit F-keys — the
 * whole point of this change — would have turned it into phantom keystrokes.
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

#define EV_RING 128u
static volatile struct key_ev g_ev[EV_RING];
static volatile uint16_t g_ev_head, g_ev_tail;

static uint8_t g_mods;      /* KMOD_* bitmask, live */
static int     g_e0;        /* the previous byte was 0xE0 — prefix pending */

/* DDR-991 §4: breaks can no longer be discarded wholesale. Without them a
 * modifier latches down forever after one press, and a phantom Ctrl turns
 * ordinary typing into control codes. */
static void mods_set(uint8_t bit, int down) {
    if (down) g_mods |= bit;
    else      g_mods &= (uint8_t)~bit;
}

static void ev_push(uint8_t code, uint8_t down, uint8_t ascii) {
    uint16_t nh = (uint16_t)((g_ev_head + 1u) & (EV_RING - 1u));
    if (nh == g_ev_tail)
        return;                                  /* drop on full, like the ASCII ring */
    g_ev[g_ev_head].code  = code;
    g_ev[g_ev_head].mods  = g_mods;              /* sampled AT the event (§3) */
    g_ev[g_ev_head].down  = down;
    g_ev[g_ev_head].ascii = ascii;
    g_ev_head = nh;
}

/* Extended (0xE0-prefixed) make codes -> keycode. 0 = not one we report. */
static uint8_t e0_key(uint8_t sc) {
    switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        default:   return 0;
    }
}

void ps2kbd_isr(void) {
    uint8_t sc = inb(0x60);

    /* Prefix byte: consume it and decode the NEXT byte as extended. Must come
     * before the break-code test — 0xE0 has bit 7 set and would otherwise be
     * mistaken for a break (§1). */
    if (sc == 0xE0) { g_e0 = 1; return; }

    int down = !(sc & 0x80);
    uint8_t base = (uint8_t)(sc & 0x7F);
    int e0 = g_e0;
    g_e0 = 0;

    if (e0) {
        /* Right-hand modifiers arrive here; they share the left-hand base code,
         * which is exactly why the prefix must not be lost. */
        switch (base) {
            case 0x1D: mods_set(KMOD_CTRL_R, down); mods_set(KMOD_CTRL, down);
                       ev_push(KEY_CTRL, (uint8_t)down, 0); return;
            case 0x38: mods_set(KMOD_ALT_R, down);  mods_set(KMOD_ALT, down);
                       ev_push(KEY_ALT, (uint8_t)down, 0);  return;
            case 0x5B: mods_set(KMOD_META, down);
                       ev_push(KEY_META, (uint8_t)down, 0); return;
            case 0x5C: mods_set(KMOD_META_R, down); mods_set(KMOD_META, down);
                       ev_push(KEY_META, (uint8_t)down, 0); return;
            default: break;
        }
        uint8_t k = e0_key(base);
        if (k)
            ev_push(k, (uint8_t)down, 0);
        return;
    }

    /* Left-hand modifiers. */
    switch (base) {
        case 0x2A: mods_set(KMOD_SHIFT, down);   ev_push(KEY_SHIFT, (uint8_t)down, 0); return;
        case 0x36: mods_set(KMOD_SHIFT_R, down); mods_set(KMOD_SHIFT, down);
                   ev_push(KEY_SHIFT, (uint8_t)down, 0); return;
        case 0x1D: mods_set(KMOD_CTRL, down);    ev_push(KEY_CTRL, (uint8_t)down, 0);  return;
        case 0x38: mods_set(KMOD_ALT, down);     ev_push(KEY_ALT, (uint8_t)down, 0);   return;
        default: break;
    }

    /* Function keys: F1..F10 are 0x3B..0x44, then F11/F12 at 0x57/0x58 — the
     * discontinuity is real, not a typo. These were dropped outright by the old
     * `sc >= 0x40` cap. */
    if (base >= 0x3B && base <= 0x44) { ev_push((uint8_t)(KEY_F1 + (base - 0x3B)), (uint8_t)down, 0); return; }
    if (base == 0x57) { ev_push((uint8_t)(KEY_F1 + 10), (uint8_t)down, 0); return; }
    if (base == 0x58) { ev_push((uint8_t)(KEY_F1 + 11), (uint8_t)down, 0); return; }

    if (base >= 0x40)
        return;                                  /* nothing else above the map */

    char c = (g_mods & KMOD_SHIFT) ? map_upper[base] : map_lower[base];
    if (!c)
        return;

    ev_push((uint8_t)c, (uint8_t)down, (uint8_t)c);

    /* The DDR-703 byte stream carries MAKE codes only, exactly as before — a
     * consumer of NSI 46 must not start seeing each key twice. */
    if (!down)
        return;
    /* DDR-992 §2: a chord is not text. Ctrl+C is an interrupt, not the letter
     * 'c'; Super+M is a mode toggle, not the letter 'm'. Without this, one
     * keypress arrives twice — as a chord on NSI 96 and as a bare letter on
     * NSI 46 — and a consumer acts on both, which is how Super+M would flip the
     * mode and then immediately have the plain-'m' branch force it back.
     * Special-casing 'm' cannot work: the byte stream carries no modifier
     * state, which is why DDR-991 added a second ring at all.
     * SHIFT is deliberately excluded — Shift IS a text modifier, and selecting
     * the shifted glyph is exactly what map_upper is for. */
    if (g_mods & (KMOD_CTRL | KMOD_ALT | KMOD_META))
        return;
    uint16_t nh = (uint16_t)((g_head + 1) & (KBD_RING - 1));
    if (nh != g_tail) {                          /* drop on full */
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

int ps2kbd_pop_ev(struct key_ev *buf, int max) {
    int n = 0;
    while (n < max && g_ev_tail != g_ev_head) {
        buf[n++] = g_ev[g_ev_tail];
        g_ev_tail = (uint16_t)((g_ev_tail + 1u) & (EV_RING - 1u));
    }
    return n;
}
