/* user/term.c — a PRISM terminal window (DDR-1027).
 *
 * Ctrl+Alt+T in the compositor forks and execve's this program. It owns one
 * surface, runs PRISM as a child over a pipe pair, renders what PRISM writes
 * into the surface with the Inter atlas, and forwards the keys the compositor
 * routes to the focused window back into PRISM's stdin.
 *
 * The shape worth knowing before reading: this is an EPOLL client, not a
 * straight-line reader. There is no O_NONBLOCK and no fcntl in this kernel, so
 * a plain read() on PRISM's stdout pipe would block whenever PRISM had nothing
 * to say -- which is almost always -- and the window would stop draining its
 * own key ring while it waited. SYS_EPOLL_WAIT with timeout 0 answers "is there
 * a byte?" without committing to wait for one. See DDR-1027 §2.
 */
#include <stdio.h>

#include "inter_font.h"

#define SYS_YIELD             3
#define SYS_EXIT              4
#define SYS_READ              5
#define SYS_WRITE             6
#define SYS_CLOSE             8
#define SYS_EXECVE           14
#define SYS_FORK             15
#define SYS_PIPE             17
#define SYS_DUP2             18
#define SYS_EPOLL_CREATE     19
#define SYS_EPOLL_CTL        20
#define SYS_EPOLL_WAIT       21
#define SYS_SURFACE_CREATE   48
#define SYS_SURFACE_MAP      49
#define SYS_SURFACE_COMMIT   50
#define SYS_SURFACE_RAISE    54
#define SYS_SURFACE_GETKEY   56
#define SYS_SURFACE_SET_TITLE 61

#define EPOLLIN        0x001u
#define EPOLL_CTL_ADD  1

/* Mirror of kernel/proc/epoll.c's user record -- packed, 12 bytes. */
struct epoll_event {
    unsigned int       events;
    unsigned long long data;
} __attribute__((packed));

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

/* A fixed 9 px cell rather than Inter's proportional advance. Inter is not a
 * monospace face, so honouring adv would give a terminal whose columns do not
 * line up between rows -- and a shell's output is column-aligned by assumption
 * (`ls`, `ps`). The glyph is still drawn with its own xoff inside the cell. */
#define CELL_W   9
#define CELL_H  18
#define COLS    52
#define ROWS    16
#define TERM_W  (COLS * CELL_W)          /* 468 */
#define TERM_H  (ROWS * CELL_H)          /* 288 */

/* Terminal palette: near-black ground, warm off-white text. */
#define BG_B 0x18
#define BG_G 0x14
#define BG_R 0x12
#define FG_B 0xE0
#define FG_G 0xE8
#define FG_R 0xEC

static unsigned char *g_va;              /* surface BGRA buffer */
static char g_grid[ROWS][COLS];
static int  g_cx, g_cy;

static void px(int x, int y, unsigned a) {
    if (x < 0 || y < 0 || x >= TERM_W || y >= TERM_H)
        return;
    unsigned char *p = g_va + ((unsigned)y * TERM_W + (unsigned)x) * 4u;
    /* Integer alpha blend against the terminal ground. No float: this runs per
     * glyph pixel on every redraw, and the compositor's float blend_px writes
     * the global framebuffer, not a surface, so it is not reusable here. */
    p[0] = (unsigned char)((FG_B * a + BG_B * (255u - a)) / 255u);
    p[1] = (unsigned char)((FG_G * a + BG_G * (255u - a)) / 255u);
    p[2] = (unsigned char)((FG_R * a + BG_R * (255u - a)) / 255u);
    p[3] = 0xFF;
}

static void draw_cell(char ch, int col, int row) {
    unsigned c = (unsigned char)ch;
    if (c < INTER_FIRST || c > INTER_LAST)
        return;
    const struct inter_glyph *gl = &inter_glyphs[c - INTER_FIRST];
    int ox = col * CELL_W;
    int oy = row * CELL_H;
    for (int gy = 0; gy < gl->h; gy++)
        for (int gx = 0; gx < gl->w; gx++) {
            unsigned char a = inter_pixels[gl->off + gy * gl->w + gx];
            if (a)
                px(ox + gl->xoff + gx, oy + INTER_ASCENT + gl->yoff + gy, a);
        }
}

static void redraw(void) {
    for (unsigned i = 0; i < (unsigned)TERM_W * TERM_H; i++) {
        g_va[i * 4 + 0] = BG_B;
        g_va[i * 4 + 1] = BG_G;
        g_va[i * 4 + 2] = BG_R;
        g_va[i * 4 + 3] = 0xFF;
    }
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (g_grid[r][c])
                draw_cell(g_grid[r][c], c, r);
}

static void scroll_up(void) {
    for (int r = 0; r + 1 < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            g_grid[r][c] = g_grid[r + 1][c];
    for (int c = 0; c < COLS; c++)
        g_grid[ROWS - 1][c] = 0;
}

static void newline(void) {
    g_cx = 0;
    if (++g_cy >= ROWS) { g_cy = ROWS - 1; scroll_up(); }
}

/* No ANSI, no cursor addressing (DDR-1027 §5). PRISM emits plain lines; a VT
 * parser is separate work this deliberately does not start. */
static void put_ch(char ch) {
    if (ch == '\n') { newline(); return; }
    if (ch == '\r') { g_cx = 0; return; }
    if (ch == '\b') { if (g_cx > 0) g_grid[g_cy][--g_cx] = 0; return; }
    if (ch == '\t') { g_cx = (g_cx + 8) & ~7; if (g_cx >= COLS) newline(); return; }
    if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E)
        return;
    if (g_cx >= COLS) newline();
    g_grid[g_cy][g_cx++] = ch;
}

int main(void) {
    int to_sh[2], from_sh[2];
    if (nsi(SYS_PIPE, (long)to_sh, 0, 0) != 0 ||
        nsi(SYS_PIPE, (long)from_sh, 0, 0) != 0) {
        printf("PRADYOS_TERM_FAIL pipe\n");
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }

    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid == 0) {
        nsi(SYS_DUP2, to_sh[0], 0, 0);
        nsi(SYS_DUP2, from_sh[1], 1, 0);
        nsi(SYS_CLOSE, to_sh[1], 0, 0);
        nsi(SYS_CLOSE, from_sh[0], 0, 0);
        nsi(SYS_EXECVE, (long)"/PRISM.ELF", 0, 0);
        nsi(SYS_EXIT, 127, 0, 0);            /* execve failed: give up */
    }
    if (kid < 0) {
        printf("PRADYOS_TERM_FAIL fork rc=%ld\n", kid);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    nsi(SYS_CLOSE, to_sh[0], 0, 0);
    nsi(SYS_CLOSE, from_sh[1], 0, 0);

    long id = nsi(SYS_SURFACE_CREATE, TERM_W, TERM_H, 0);
    if (id < 0) {
        printf("PRADYOS_TERM_FAIL create rc=%ld\n", id);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    long va = nsi(SYS_SURFACE_MAP, id, 0, 0);
    if (va < 0) {
        printf("PRADYOS_TERM_FAIL map rc=%ld\n", va);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    g_va = (unsigned char *)va;
    redraw();
    nsi(SYS_SURFACE_SET_TITLE, id, (long)"PRISM", 0);
    nsi(SYS_SURFACE_COMMIT, id, 260, 120);
    nsi(SYS_SURFACE_RAISE, id, 0, 0);        /* top + focused: keys route here */
    printf("PRADYOS_TERM_OK id=%ld pid=%ld\n", id, kid);
    fflush(stdout);

    long epfd = nsi(SYS_EPOLL_CREATE, 8, 0, 0);
    struct epoll_event ev = { EPOLLIN, 0 };
    if (epfd < 0 || nsi4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, from_sh[0], (long)&ev) != 0) {
        printf("PRADYOS_TERM_FAIL epoll rc=%ld\n", epfd);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }

    unsigned long rx = 0, tx = 0;
    int said_rx = 0, said_tx = 0, said_stat = 0;
    for (;;) {
        int dirty = 0;

        /* Keys the compositor routed to the focused window -> PRISM's stdin. */
        for (;;) {
            long c = nsi(SYS_SURFACE_GETKEY, id, 0, 0);
            if (c < 0)
                break;
            char ch = (char)c;
            nsi(SYS_WRITE, to_sh[1], (long)&ch, 1);
            tx++;
            if (!said_tx) {
                said_tx = 1;
                printf("PRADYOS_TERM_TX ch=%d\n", (int)(unsigned char)ch);
                fflush(stdout);
            }
        }

        /* PRISM's stdout -> the grid. Timeout 0: ask, never wait (§2). */
        struct epoll_event out;
        if (nsi4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 0) > 0) {
            char buf[128];
            long n = nsi(SYS_READ, from_sh[0], (long)buf, (long)sizeof buf);
            for (long i = 0; i < n; i++)
                put_ch(buf[i]);
            if (n > 0) {
                rx += (unsigned long)n;
                dirty = 1;
                if (!said_rx) {
                    said_rx = 1;
                    printf("PRADYOS_TERM_RX n=%ld first=%d\n",
                           n, (int)(unsigned char)buf[0]);
                    fflush(stdout);
                }
            }
        }

        /* Diagnostic, NOT a gate arm: rx>0 && tx>0 is implied by TERM_RX and
         * TERM_TX both having fired, so asserting it would be decoration. It is
         * printed because the totals are what a future failure would need --
         * "keys went in and nothing came back" reads very differently from
         * "neither direction moved". */
        if (!said_stat && rx && tx) {
            said_stat = 1;
            printf("PRADYOS_TERM_STAT rx=%lu tx=%lu\n", rx, tx);
            fflush(stdout);
        }

        if (dirty) {
            redraw();
            nsi(SYS_SURFACE_COMMIT, id, 260, 120);
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
}
