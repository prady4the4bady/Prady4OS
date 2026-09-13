/* user/include/uline.h — build a whole measured line, then emit it with ONE write.
 *
 * DDR-1056, and it is DDR-1055's defect one ring out. A probe that does
 *
 *     wr("PRADYOS_X_OK id="); wrdec(id); wr(" st="); wrdec(st); wr("\n");
 *
 * makes FIVE write(2) calls, hence five separate `g_console_lock` acquisitions
 * in the kernel, with four gaps. Every other printer in the system — the [hb]
 * heartbeat, a driver, another probe on another CPU — can occupy a gap. The
 * kernel emits every character the probe intended and the gate's whole-line
 * grep matches nothing, which is what `[actiondel] FAIL — no measured line in
 * the capture` looks like on a boot that PASSed.
 *
 * Build the line here and hand `ul_end()` to the probe's own single-argument
 * writer: one call, one write, one console-lock acquisition, atomic against
 * every printer in the tree.
 *
 * Truncation is LOUD rather than silent — a shortened sentinel is the same
 * failure this removes — so an overflowing line ends in "[uline] TRUNC", which
 * is in GLOBAL_FORBIDDEN.
 */
#ifndef PRADYOS_ULINE_H
#define PRADYOS_ULINE_H

#define ULINE_MAX 256

typedef struct { char b[ULINE_MAX]; int n; int trunc; } uline;

static inline void ul_init(uline *u) { u->n = 0; u->trunc = 0; }

static inline void ul_c(uline *u, char c) {
    /* -16 leaves room for the TRUNC marker and the NUL, so the marker can
     * always be written; a truncation that itself got truncated would be the
     * silent failure this exists to prevent. */
    if (u->n >= ULINE_MAX - 16) { u->trunc = 1; return; }
    u->b[u->n++] = c;
}

static inline void ul_s(uline *u, const char *s) {
    if (!s) s = "(null)";
    while (*s) ul_c(u, *s++);
}

/* SIGNED: gates match st=-1, rc=-40, n=-*[0-9]*. An unsigned-only helper would
 * print those as 20-digit numbers and no gate pattern would match. */
static inline void ul_d(uline *u, long v) {
    char t[24];
    int i = 0;
    unsigned long m;
    if (v < 0) { ul_c(u, '-'); m = (unsigned long)(-(v + 1)) + 1UL; }
    else       { m = (unsigned long)v; }
    if (m == 0) { ul_c(u, '0'); return; }
    while (m) { t[i++] = (char)('0' + (m % 10UL)); m /= 10UL; }
    while (i) ul_c(u, t[--i]);
}

static inline void ul_x(uline *u, unsigned long v) {
    static const char hx[] = "0123456789ABCDEF";
    ul_s(u, "0x");
    for (int sh = 60; sh >= 0; sh -= 4) ul_c(u, hx[(v >> sh) & 0xFUL]);
}

/* NUL-terminate and return the buffer. Pass it to the probe's own wr(). */
static inline const char *ul_end(uline *u) {
    if (u->trunc) {
        const char *m = "[uline] TRUNC";
        while (*m && u->n < ULINE_MAX - 1) u->b[u->n++] = *m++;
    }
    u->b[u->n] = '\0';
    return u->b;
}

#endif /* PRADYOS_ULINE_H */
