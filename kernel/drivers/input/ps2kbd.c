/* kernel/drivers/input/ps2kbd.c — PS/2 keyboard scancode decode (DDR-703/991/993).
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
 *
 * DDR-993 §1 — WHY g_mods IS DERIVED, NOT ASSIGNED. DDR-991 shipped a
 * `mods_set(bit, down)` that wrote the AGGREGATE bit (KMOD_SHIFT) and the SIDE
 * bit (KMOD_SHIFT_R) with two unconditional calls. The break code of ONE side
 * therefore cleared the aggregate while the other side was still physically
 * held: press L-Shift, press R-Shift, release R-Shift, and KMOD_SHIFT reads 0
 * with L-Shift down. The result is worse than a lost Shift — the very next
 * keypress is decoded through map_lower, and for Ctrl and Alt the DDR-992 chord
 * suppression stops firing, so a still-held Ctrl silently starts typing text.
 *
 * The fix is structural rather than a conditional bolted onto the clear path:
 * only the eight PHYSICAL keys have state (g_side), and g_mods is RECOMPUTED
 * from that state on every modifier edge. An aggregate that is derived cannot
 * disagree with its sides. §2 explains why this needed the decode split.
 *
 * DDR-993 §2 — WHY THE DECODE IS SPLIT FROM THE PORT READ. `ps2kbd_isr` read
 * 0x60 directly, so the ONLY way to exercise the decoder was to make QEMU
 * deliver a real IRQ1 — and QEMU's HMP `sendkey` emits a press and its release
 * as one indivisible action. The defect above needs two keys held at once, so
 * no `sendkey` sequence can reach it: the arm CodeRabbit correctly observed was
 * missing was not merely absent, it was UNWRITABLE against the old shape. Hence
 * ps2kbd_feed(): the same decode, taking the byte as an argument, which the
 * §3 self-test drives with the exact 2A / 36 / B6 sequence. ps2kbd_isr is now
 * one line and the IRQ path is otherwise unchanged.
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

/* Physical modifier keys — the ONLY modifier state that is written directly.
 * Kernel-private: ring 3 sees KMOD_*, which is derived from this (§1). */
#define SIDE_SHIFT_L 0x01u
#define SIDE_SHIFT_R 0x02u
#define SIDE_CTRL_L  0x04u
#define SIDE_CTRL_R  0x08u
#define SIDE_ALT_L   0x10u
#define SIDE_ALT_R   0x20u
#define SIDE_META_L  0x40u
#define SIDE_META_R  0x80u

/* DDR-993 §9 — SINGLE-WRITER, and that is an IMPLICIT invariant, not a guarded
 * one. These three have no lock because exactly one CPU runs the decoder: IRQ1
 * is PIC-only today and `ioapic_route()` is never called for it. `g_e0` in
 * particular is a two-byte state machine that would tear outright under a
 * second writer — an 0xE0 landing between another CPU's prefix and its base
 * byte turns an arrow into a phantom keystroke. Group A's I/O APIC migration is
 * precisely the change that breaks this, so it must add locking here in the
 * same commit; a comment is a note, not a guard. */
static uint8_t g_side;      /* SIDE_* — which physical modifier keys are held  */
static uint8_t g_mods;      /* KMOD_* — DERIVED from g_side, never assigned ad hoc */
static int     g_e0;        /* the previous byte was 0xE0 — prefix pending */

/* DDR-991 §4: breaks can no longer be discarded wholesale. Without them a
 * modifier latches down forever after one press, and a phantom Ctrl turns
 * ordinary typing into control codes.
 * DDR-993 §1: and the aggregate must be recomputed, not cleared — an OR of the
 * two sides is the whole point, so releasing one side while the other is held
 * leaves the aggregate set. */
static void mods_edge(uint8_t side_bit, int down) {
    if (down) g_side |= side_bit;
    else      g_side &= (uint8_t)~side_bit;

    uint8_t m = 0;
    if (g_side & (SIDE_SHIFT_L | SIDE_SHIFT_R)) m |= KMOD_SHIFT;
    if (g_side & (SIDE_CTRL_L  | SIDE_CTRL_R))  m |= KMOD_CTRL;
    if (g_side & (SIDE_ALT_L   | SIDE_ALT_R))   m |= KMOD_ALT;
    if (g_side & (SIDE_META_L  | SIDE_META_R))  m |= KMOD_META;
    if (g_side & SIDE_SHIFT_R) m |= KMOD_SHIFT_R;
    if (g_side & SIDE_CTRL_R)  m |= KMOD_CTRL_R;
    if (g_side & SIDE_ALT_R)   m |= KMOD_ALT_R;
    if (g_side & SIDE_META_R)  m |= KMOD_META_R;
    g_mods = m;
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

/* Decode one scancode byte. Split out of the ISR so it can be driven directly
 * by the §3 self-test — see the header comment for why sendkey cannot. */
void ps2kbd_feed(uint8_t sc) {
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
            case 0x1D: mods_edge(SIDE_CTRL_R, down);
                       ev_push(KEY_CTRL, (uint8_t)down, 0); return;
            case 0x38: mods_edge(SIDE_ALT_R, down);
                       ev_push(KEY_ALT, (uint8_t)down, 0);  return;
            case 0x5B: mods_edge(SIDE_META_L, down);
                       ev_push(KEY_META, (uint8_t)down, 0); return;
            case 0x5C: mods_edge(SIDE_META_R, down);
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
        case 0x2A: mods_edge(SIDE_SHIFT_L, down); ev_push(KEY_SHIFT, (uint8_t)down, 0); return;
        case 0x36: mods_edge(SIDE_SHIFT_R, down); ev_push(KEY_SHIFT, (uint8_t)down, 0); return;
        case 0x1D: mods_edge(SIDE_CTRL_L, down);  ev_push(KEY_CTRL, (uint8_t)down, 0);  return;
        case 0x38: mods_edge(SIDE_ALT_L, down);   ev_push(KEY_ALT, (uint8_t)down, 0);   return;
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

    /* DDR-993 §4: `code` IDENTIFIES the physical key, `ascii` carries the glyph.
     * DDR-991 put the shifted glyph in both, so one key's make and break did not
     * pair up whenever Shift was released between them: press Shift+A delivered
     * code='A', and its break — arriving after the Shift break, as it usually
     * does — delivered code='a'. A consumer tracking held keys by `code` leaks
     * one entry per shifted keypress and never sees the release. The identity
     * must not depend on state that can change between the two edges, so it is
     * taken from the UNSHIFTED column; ascii keeps the shifted glyph, which is
     * what map_upper is for. */
    char id = map_lower[base];
    if (!id)
        return;
    char c = (g_mods & KMOD_SHIFT) ? map_upper[base] : id;
    if (!c)
        return;

    ev_push((uint8_t)id, (uint8_t)down, (uint8_t)c);

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

void ps2kbd_isr(void) {
    ps2kbd_feed(inb(0x60));
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

/* ---- DDR-993 §3: paired-modifier self-test --------------------------------
 * Returns 0 on pass, else the number of the step that failed. Drives raw
 * scancodes through ps2kbd_feed, because the sequence it must produce — two
 * keys of one pair held simultaneously, then ONE released — cannot be expressed
 * with QEMU's `sendkey`, which couples every press to its own release. That is
 * why this is a kernel arm rather than another arm of the ring-3 probe.
 *
 * Non-perturbing: all decoder state is saved and restored, so a boot that runs
 * this is indistinguishable from one that does not. It runs before the probe is
 * spawned and before any key is injected, but restoring anyway means an
 * unrelated caller cannot be broken by it later.
 *
 * MUTATION CHECK (DDR-993 §5): with mods_edge reverted to DDR-991's
 * unconditional two-call clear, step 3 fails — that is the assertion this test
 * exists for, and no other arm in smoke-modkeys reaches it. */
int ps2kbd_selftest(void) {
    uint8_t  s_side = g_side, s_mods = g_mods;
    int      s_e0 = g_e0;
    uint16_t s_eh = g_ev_head, s_et = g_ev_tail, s_h = g_head, s_t = g_tail;
    int rc = 0;

    g_side = 0; g_mods = 0; g_e0 = 0;

    /* --- Shift pair: the exact DDR-993 §1 sequence. --- */
    ps2kbd_feed(0x2A);                                   /* L-Shift make  */
    if (!(g_mods & KMOD_SHIFT)) { rc = 1; goto out; }
    ps2kbd_feed(0x36);                                   /* R-Shift make  */
    if (!(g_mods & KMOD_SHIFT) || !(g_mods & KMOD_SHIFT_R)) { rc = 2; goto out; }
    ps2kbd_feed(0xB6);                                   /* R-Shift break */
    /* L-Shift is STILL HELD. Under DDR-991 this read 0. */
    if (!(g_mods & KMOD_SHIFT)) { rc = 3; goto out; }
    if (g_mods & KMOD_SHIFT_R)  { rc = 4; goto out; }    /* side must clear */
    ps2kbd_feed(0xAA);                                   /* L-Shift break */
    if (g_mods & KMOD_SHIFT)    { rc = 5; goto out; }    /* now, and only now */

    /* --- Ctrl pair, across the 0xE0 prefix: same defect, different path.
     * This one is the dangerous half — a Ctrl that reads clear while held
     * disables the DDR-992 chord suppression, so the chord types text. --- */
    ps2kbd_feed(0x1D);                                   /* L-Ctrl make       */
    if (!(g_mods & KMOD_CTRL)) { rc = 6; goto out; }
    ps2kbd_feed(0xE0); ps2kbd_feed(0x1D);                /* R-Ctrl make       */
    if (!(g_mods & KMOD_CTRL) || !(g_mods & KMOD_CTRL_R)) { rc = 7; goto out; }
    ps2kbd_feed(0xE0); ps2kbd_feed(0x9D);                /* R-Ctrl break      */
    if (!(g_mods & KMOD_CTRL)) { rc = 8; goto out; }
    ps2kbd_feed(0x9D);                                   /* L-Ctrl break      */
    if (g_mods & KMOD_CTRL)    { rc = 9; goto out; }

    /* --- §4: make/break of one physical key agree on `code` even when Shift is
     * released in between. Under DDR-991 the break reported 'a' for a key whose
     * make reported 'A'. --- */
    {
        struct key_ev e[8];
        int n;
        g_ev_head = g_ev_tail = 0;
        ps2kbd_feed(0x2A);                               /* Shift down        */
        ps2kbd_feed(0x1E);                               /* 'a' make, shifted */
        ps2kbd_feed(0xAA);                               /* Shift up          */
        ps2kbd_feed(0x9E);                               /* 'a' break         */
        n = ps2kbd_pop_ev(e, 8);
        if (n != 4)                        { rc = 10; goto out; }
        if (e[1].code != 'a' || e[1].ascii != 'A') { rc = 11; goto out; }
        if (e[3].code != 'a')              { rc = 12; goto out; }
    }

out:
    g_side = s_side; g_mods = s_mods; g_e0 = s_e0;
    g_ev_head = s_eh; g_ev_tail = s_et; g_head = s_h; g_tail = s_t;
    return rc;
}
