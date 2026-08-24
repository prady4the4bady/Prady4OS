/* user/surfacetest.c — two client windows + focus/key routing (DDR-706/708).
 *
 * Creates two overlapping 64x64 surfaces (A green, B blue), commits both, and
 * RAISEs B (top + focused). Then drains each surface's key ring: the compositor
 * forwards keystrokes to the focused window (B), which prints PRADYOS_FOCUS_KEY.
 */
#include <stdio.h>

#define SYS_EXIT            4
#define SYS_YIELD           3
#define SYS_SURFACE_CREATE  48
#define SYS_SURFACE_MAP     49
#define SYS_SURFACE_COMMIT  50
#define SYS_SURFACE_RAISE   54
#define SYS_SURFACE_GETKEY  56
#define SYS_SURFACE_CLOSE   59
#define SYS_SURFACE_RESIZE  60
#define SYS_SURFACE_SET_TITLE 61
#define SYS_SURFACE_GETEV     63

#define SYS_TIME              72     /* DDR-749: (struct rtc_time*) -> 0 | -EFAULT */

struct surf_event { unsigned short type, arg0, arg1; };   /* type 1 = RESIZE_REQ */

/* Mirror of kernel struct rtc_time (rtc.h) — DDR-749. */
struct rtc_time {
    unsigned short year;
    unsigned char  month, day, hour, minute, second;
};

/* DDR-911: how long C stays clickable after the compositor first composites it.
 * smoke-wmclose's injector lands its click in about a second; smoke-winops has a
 * 90s budget to observe the shrink, so four seconds satisfies both. */
#define GRACE_SECS 4

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* DDR-911: wall-clock seconds-of-day for the grace period below. A REAL clock,
 * never a loop counter — a loop counter is what raced the compositor and cost
 * this entire investigation. */
static long wall_secs(void) {
    struct rtc_time t;
    if (nsi(SYS_TIME, (long)&t, 0, 0) != 0)
        return -1;
    return (long)t.hour * 3600 + (long)t.minute * 60 + (long)t.second;
}

static long make_window(unsigned char b, unsigned char g, unsigned char r, int x, int y) {
    long id = nsi(SYS_SURFACE_CREATE, 64, 64, 0);
    if (id < 0) return id;
    long va = nsi(SYS_SURFACE_MAP, id, 0, 0);
    if (va < 0) return va;
    unsigned char *s = (unsigned char *)va;
    for (unsigned i = 0; i < 64u * 64u; i++) {
        s[i * 4 + 0] = b; s[i * 4 + 1] = g; s[i * 4 + 2] = r; s[i * 4 + 3] = 0xFF;
    }
    nsi(SYS_SURFACE_COMMIT, id, x, y);
    return id;
}

int main(void) {
    long a = make_window(0x40, 0xE0, 0x40, 100, 100);   /* green, bottom */
    long b = make_window(0xE0, 0x40, 0x40, 140, 140);   /* blue,  raised */
    if (a < 0 || b < 0) {
        printf("PRADYOS_SURFACE_CLIENT_FAIL a=%ld b=%ld\n", a, b);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    nsi(SYS_SURFACE_RAISE, b, 0, 0);                     /* B on top + focused */
    printf("PRADYOS_SURFACE_CLIENT_OK a=%ld b=%ld\n", a, b);
    fflush(stdout);

    /* DDR-715: name the windows (glyph-covered alphabet); the compositor draws
     * the title in the title bar. */
    if (nsi(SYS_SURFACE_SET_TITLE, a, (long)"ALPHA", 0) == 0 &&
        nsi(SYS_SURFACE_SET_TITLE, b, (long)"BETA", 0) == 0) {
        printf("PRADYOS_TITLE_OK\n");
        fflush(stdout);
    }

    /* DDR-711: a third window C exercises resize + close. C is created and resized
     * (64x64 -> 96x96) up front, but deliberately NOT raised — B keeps focus, so
     * smoke-focus/smoke-drag are unaffected. C is placed off to the side so it
     * never sits under B's title bar. It persists so the compositor composites it
     * (the live set grows to 3); after a spell we close it and the set shrinks back
     * to 2, which the compositor reports (PRADYOS_SURFACE_GONE). Meanwhile keep
     * draining the focused window's key ring (DDR-708). */
    long c = make_window(0x40, 0x40, 0xE0, 420, 70);        /* red, off to the side */
    if (c >= 0)
        nsi(SYS_SURFACE_SET_TITLE, c, (long)"GAMMA", 0);    /* DDR-715 */
    if (c >= 0 && nsi(SYS_SURFACE_RESIZE, c, 96, 96) == 0) {
        long cva = nsi(SYS_SURFACE_MAP, c, 0, 0);           /* re-map the new 96x96 buffer */
        if (cva > 0) {
            unsigned char *s = (unsigned char *)cva;
            for (unsigned i = 0; i < 96u * 96u; i++) {
                s[i*4+0] = 0x40; s[i*4+1] = 0x40; s[i*4+2] = 0xE0; s[i*4+3] = 0xFF;
            }
        }
        printf("PRADYOS_RESIZE_OK id=%ld\n", c);
        fflush(stdout);
    }

    int closed = 0;
    long grace_start = -1;          /* DDR-911: wall-clock start of C's grace */
    for (;;) {
        long ka = nsi(SYS_SURFACE_GETKEY, a, 0, 0);
        /* DDR-995: `code=` added because a literal tab is not greppable, and
         * arm B asserts that a PLAIN Tab now reaches the focused application —
         * which DDR-720's unconditional hotkey made impossible. smoke-focus
         * matches only the bare PRADYOS_FOCUS_KEY prefix, so it is unaffected. */
        if (ka >= 0) { printf("PRADYOS_FOCUS_KEY id=%ld ch=%c code=%ld\n", a, (char)ka, ka); fflush(stdout); }
        long kb = nsi(SYS_SURFACE_GETKEY, b, 0, 0);
        if (kb >= 0) { printf("PRADYOS_FOCUS_KEY id=%ld ch=%c code=%ld\n", b, (char)kb, kb); fflush(stdout); }

        /* DDR-718: honor compositor resize requests on B — resize, re-map,
         * redraw at the new size, re-commit at the same position. */
        struct surf_event ev;
        ev.type = 0;
        if (nsi(SYS_SURFACE_GETEV, b, (long)&ev, 0) == 0 && ev.type == 2) {
            /* DDR-725: scroll event — delta in arg1 (signed 16-bit). */
            printf("PRADYOS_EV_SCROLL_OK d=%d\n", (int)(short)ev.arg1);
            fflush(stdout);
        }
        if (ev.type == 1) {
            if (nsi(SYS_SURFACE_RESIZE, b, ev.arg0, ev.arg1) == 0) {
                long bva = nsi(SYS_SURFACE_MAP, b, 0, 0);
                if (bva > 0) {
                    unsigned char *s = (unsigned char *)bva;
                    for (unsigned i = 0; i < (unsigned)ev.arg0 * ev.arg1; i++) {
                        s[i*4+0] = 0xE0; s[i*4+1] = 0x40; s[i*4+2] = 0x40; s[i*4+3] = 0xFF;
                    }
                }
                /* Re-commit keeping the compositor-owned position (DDR-719):
                 * 0x7FFFFFFF = SURF_POS_KEEP. */
                nsi(SYS_SURFACE_COMMIT, b, 0x7FFFFFFF, 0);
                printf("PRADYOS_EV_RESIZE_OK w=%u h=%u\n", ev.arg0, ev.arg1);
                fflush(stdout);
            }
        }

        /* DDR-911: close C only once the compositor has DEMONSTRABLY composited
         * it (surf_event type 3), so the live set is observed at 3 before it
         * shrinks to 2.
         *
         * This replaced `ticks > 12000`, which measured a duration in loop
         * iterations — a scheduling-dependent quantity. Item 16's fair-share
         * pick changed this process's CPU share and C began dying before the
         * compositor's first poll ever ran (measured: FIRSTPOLL ns=2 at HEAD
         * vs ns=3 at bd58545). The threshold had already been widened once for
         * this same race, which is proof that a bigger number postpones it
         * rather than fixing it.
         *
         * There is deliberately NO tick fallback. A fallback would reintroduce
         * the identical race on faster hardware. */
        if (!closed && c >= 0) {
            struct surf_event cev;
            cev.type = 0;
            if (nsi(SYS_SURFACE_GETEV, c, (long)&cev, 0) == 0 && cev.type == 3 &&
                grace_start < 0)
                grace_start = wall_secs();   /* composited — start the grace, don't close */

            /* DDR-911: `ticks > 12000` was silently doing TWO jobs — waiting for
             * the compositor (now the type-3 event above) AND leaving a window
             * for smoke-wmclose's click to land in. The second job was never
             * written down, so removing the counter removed it and 49 correct
             * clicks hit a surface that had already gone.
             *
             * C therefore stays alive a bounded while after being composited. If
             * the compositor closes it first on a click, this branch never runs
             * and wmclose is satisfied; otherwise C self-closes and winops is.
             *
             * Measured against a real clock, so no scheduling change can move it.
             * No liveness fallback: a clean-room bisect showed smoke-shell passes
             * 2/2 at this commit, so nothing here needs a headless escape hatch,
             * and the LIVENESS_SECS attempt was a reaction to a stale-build
             * artifact rather than to any real failure. */
            if (grace_start >= 0) {
                long e = wall_secs() - grace_start;
                if (e < 0) e += 24 * 3600;               /* midnight wrap */
                if (e >= GRACE_SECS) {
                    nsi(SYS_SURFACE_CLOSE, c, 0, 0);
                    printf("PRADYOS_CLOSE_OK id=%ld\n", c);
                    fflush(stdout);
                    closed = 1;
                }
            }
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}
