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

static long sys_input_poll(long a1, long a2, long a3, long a4) {
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

static long sys_mouse_poll(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    struct mouse_state ms;
    int x, y;
    uint32_t btn;
    if (virtio_input_state(&x, &y, &btn) != 0)
        return -ENODEV;
    ms.x = x; ms.y = y; ms.buttons = btn;
    ms.wheel = virtio_input_wheel();      /* DDR-725: detents since last poll */
    if (copyout((void __user *)a1, &ms, sizeof ms) < 0)
        return -EFAULT;
    return 0;
}

void sys_input_register(void) {
    syscall_register(SYS_INPUT_POLL, sys_input_poll);
    syscall_register(SYS_MOUSE_POLL, sys_mouse_poll);
}
