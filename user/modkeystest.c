/* user/modkeystest.c — PS/2 modifier / extended-key probe (DDR-991 §6).
 *
 * Announces PRADYOS_MODKEYS_WAIT, then drains BOTH input NSIs while the harness
 * injects real keys through QEMU's HMP `sendkey` (genuine IRQ1 path, same as
 * smoke-input). Five arms, each targeting something the DDR-703 driver could
 * not do:
 *
 *   A  plain 'a' still arrives on SYS_INPUT_POLL (46)  — the OLD ABI is intact.
 *      This arm is why the byte stream was left alone: PRISM and the shell read
 *      it, and a chord has no business in a byte stream.
 *   B  F1 arrives as KEY_F1 with ascii=0 — the old `sc >= 0x40` cap dropped
 *      every function key outright.
 *   C  Arrow-Up arrives as KEY_UP. This cannot pass without 0xE0 prefix
 *      decoding: an arrow is `E0 48`, and the old code swallowed the prefix as
 *      a break code and then dropped 0x48 for being >= 0x40. Arrows are
 *      literally invisible today, so their arrival IS the assertion.
 *   D  Ctrl+C reports KMOD_CTRL set IN THE SAME EVENT as the 'c' — not read
 *      afterwards, which would race the release (DDR-991 §3).
 *   E  a later plain key reports mods == 0. This is the §4 release edge, and it
 *      is the arm that matters most: without break-code handling a modifier
 *      latches down forever after one press, and a phantom Ctrl silently turns
 *      ordinary typing into control codes. A latched-modifier regression passes
 *      every other arm here.
 *
 * Freestanding (no libc, user.ld, no writable globals per DDR-826).
 */

#define SYS_EXIT          4
#define SYS_WRITE         6
#define SYS_YIELD         3
#define SYS_INPUT_POLL    46
#define SYS_KEY_POLL      96

/* Must match kernel/drivers/input/ps2kbd.h — this is the NSI 96 ABI. */
#define KMOD_SHIFT  0x01u
#define KMOD_CTRL   0x02u
#define KEY_F1      0x80u
#define KEY_UP      0x90u

struct key_ev { unsigned char code, mods, down, ascii; };

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int a_ascii = 0, b_f1 = 0, c_up = 0, d_ctrl_c = 0, e_released = 0;
    int saw_ctrl_down = 0;

    wr("PRADYOS_MODKEYS_WAIT\n");

    /* Bounded poll: the injector sends four rounds with 0.25 s spacing, so this
     * window comfortably covers it without depending on arrival order. */
    for (int spin = 0; spin < 400000; spin++) {
        char cbuf[64];
        long n = nsi(SYS_INPUT_POLL, (long)cbuf, sizeof cbuf, 0);
        for (long i = 0; i < n; i++)
            if (cbuf[i] == 'a') a_ascii = 1;          /* arm A */

        struct key_ev evs[32];
        long m = nsi(SYS_KEY_POLL, (long)evs, 32, 0);
        for (long i = 0; i < m; i++) {
            struct key_ev e = evs[i];
            if (!e.down)
                continue;                              /* arms below are about presses */
            if (e.code == KEY_F1 && e.ascii == 0) b_f1 = 1;               /* arm B */
            if (e.code == KEY_UP && e.ascii == 0)  c_up = 1;              /* arm C */
            if (e.code == 'c' && (e.mods & KMOD_CTRL)) {                  /* arm D */
                d_ctrl_c = 1;
                saw_ctrl_down = 1;
            }
            /* arm E: a printable key with NO modifiers, seen after we have
             * already observed a Ctrl chord — so it proves the release edge
             * rather than merely that an unmodified key exists. */
            if (saw_ctrl_down && e.ascii != 0 && e.mods == 0)
                e_released = 1;
        }

        if (a_ascii && b_f1 && c_up && d_ctrl_c && e_released)
            break;
        nsi(SYS_YIELD, 0, 0, 0);
    }

    if (!a_ascii)   { wr("MODKEYS FAIL: arm A — plain 'a' never arrived on NSI 46 (old ABI broken)\n"); nsi(SYS_EXIT, 1, 0, 0); }
    if (!b_f1)      { wr("MODKEYS FAIL: arm B — F1 never arrived (0x40 cap still dropping it)\n");      nsi(SYS_EXIT, 1, 0, 0); }
    if (!c_up)      { wr("MODKEYS FAIL: arm C — Arrow-Up never arrived (0xE0 prefix not decoded)\n");   nsi(SYS_EXIT, 1, 0, 0); }
    if (!d_ctrl_c)  { wr("MODKEYS FAIL: arm D — Ctrl+C had no KMOD_CTRL in its own event\n");           nsi(SYS_EXIT, 1, 0, 0); }
    if (!e_released){ wr("MODKEYS FAIL: arm E — no unmodified key after Ctrl; modifier latched\n");     nsi(SYS_EXIT, 1, 0, 0); }

    wr("PRADYOS_MODKEYS_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
