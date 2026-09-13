/* kernel/syscall/sys_input.c — ring-3 input polling NSI (Layer 7, DDR-703).
 *
 * SYS_INPUT_POLL drains the PS/2 keyboard ring into a user buffer (non-blocking).
 * A ring-3 reader polls it, yielding between calls.
 */
#include "syscall.h"
#include "uaccess.h"
#include "errno.h"
#include "ps2kbd.h"
#include "virtio_input.h"

#define INPUT_MAX 256

/* DDR-725: `wheel` appended (detents since the last poll, read-and-clear).
 * We own every caller (compositor); all rebuilt together. */
struct mouse_state { int32_t x, y; uint32_t buttons; int32_t wheel; };

static long sys_input_poll(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0)
        return 0;
    if (max > INPUT_MAX)
        max = INPUT_MAX;
    char kbuf[INPUT_MAX];
    int n = ps2kbd_pop(kbuf, max);
    if (n > 0 && copyout((void __user *)a1, kbuf, (size_t)n) < 0)
        return -EFAULT;
    return n;
}

/* DDR-991: structured key events — keycode + modifier state sampled AT the
 * event. SYS_INPUT_POLL above is untouched and still carries printable ASCII
 * make codes only, so its existing consumers are unaffected; a chord simply has
 * no representation in a byte stream. */
#define KEYEV_MAX 64

static long sys_key_poll(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0)
        return 0;
    if (max > KEYEV_MAX)
        max = KEYEV_MAX;
    struct key_ev evs[KEYEV_MAX];
    int n = ps2kbd_pop_ev(evs, max);
    if (n > 0 && copyout((void __user *)a1, evs, (size_t)n * sizeof evs[0]) < 0)
        return -EFAULT;
    return n;
}

/* DDR-1025: how many times ring 3 has ASKED for pointer state, and how many of
 * those answers carried a button down. The heartbeat already prints btnedge --
 * press edges the DRIVER saw -- and a CI failure of smoke-mouse showed
 * btnedge=5 with ZERO PRADYOS_BTN_STATE lines from the compositor across ~50 s.
 * That leaves exactly two families, and these two counters separate them:
 *
 *   mpoll frozen        -> ring 3 stopped asking. A compositor liveness problem.
 *   mpoll rising, mbtn 0 -> it asked and the answer had no button. The state was
 *                           lost between the driver's edge count and this read.
 *
 * Counted here rather than in the driver because the question is about what
 * crosses the ring boundary, which is exactly this function. */
static volatile uint32_t g_mouse_polls;
static volatile uint32_t g_mouse_poll_btn;

uint32_t sys_mouse_poll_count(void)     { return __atomic_load_n(&g_mouse_polls, __ATOMIC_RELAXED); }
uint32_t sys_mouse_poll_btn_count(void) { return __atomic_load_n(&g_mouse_poll_btn, __ATOMIC_RELAXED); }

static long sys_mouse_poll(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    __atomic_add_fetch(&g_mouse_polls, 1, __ATOMIC_RELAXED);
    struct mouse_state ms;
    int x, y;
    uint32_t btn;
    if (virtio_input_state(&x, &y, &btn) != 0)
        return -ENODEV;
    /* DDR-1026: OR in any press edge that completed since the last poll. Without
     * this a click is observable only if a poll lands inside the ~200 ms the
     * button is held, and DDR-1025 measured mpollwin=0 -- five presses, zero
     * polls inside a window, against ~1,000 polls a second. The latch is
     * read-and-clear, so the edge is reported to exactly one poll and the next
     * one sees the live state again: a down followed by an up, i.e. one click. */
    btn |= virtio_input_btn_latch();
    ms.x = x; ms.y = y; ms.buttons = btn;
    if (btn)
        __atomic_add_fetch(&g_mouse_poll_btn, 1, __ATOMIC_RELAXED);
    ms.wheel = virtio_input_wheel();      /* DDR-725: detents since last poll */
    if (copyout((void __user *)a1, &ms, sizeof ms) < 0)
        return -EFAULT;
    return 0;
}

void sys_input_register(void) {
    syscall_register(SYS_INPUT_POLL, sys_input_poll);
    syscall_register(SYS_KEY_POLL,   sys_key_poll);      /* DDR-991 */
    syscall_register(SYS_MOUSE_POLL, sys_mouse_poll);
}
