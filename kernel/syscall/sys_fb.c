/* kernel/syscall/sys_fb.c — ring-3 framebuffer surface NSI (Layer 7, DDR-702).
 *
 * Three handlers over the VirtIO-GPU framebuffer (ADR-028): query geometry, map
 * the front buffer into the caller for direct drawing, and present it. With no
 * GPU up, all three return -ENODEV so a ring-3 program degrades cleanly.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "vmm.h"
#include "pmm.h"          /* PAGE_SIZE */
#include "virtio_gpu.h"

#define FB_USER_VA 0x8700000000ull   /* below the mmap arena (VMM_MMAP_BASE) */

struct fb_info { uint32_t width, height, stride, bpp; };

static long sys_fb_info(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    uint32_t w, h, stride;
    if (!virtio_gpu_fb(&w, &h, &stride))
        return -ENODEV;
    struct fb_info fi = { w, h, stride, 32 };
    if (copyout((void __user *)a1, &fi, sizeof fi) < 0)
        return -EFAULT;
    return 0;
}

static long sys_fb_map(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    uint32_t w, h, stride;
    uint8_t *fb = virtio_gpu_fb(&w, &h, &stride);
    if (!fb)
        return -ENODEV;
    struct tcb *t = current_thread;
    uint64_t phys = (uint64_t)(uintptr_t)fb;      /* identity-mapped: phys == kvirt */
    uint64_t bytes = (uint64_t)stride * h;
    uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t flags = VMM_USER | VMM_RW | VMM_NX;  /* data surface; never executable */
    for (uint64_t i = 0; i < npages; i++) {
        if (vmm_map_in(t->cr3, FB_USER_VA + i * PAGE_SIZE,
                       phys + i * PAGE_SIZE, flags) != 0)
            return -ENOMEM;                       /* partial map: caller won't use it */
    }
    return (long)FB_USER_VA;
}

static long sys_fb_flush(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (!virtio_gpu_fb(0, 0, 0))
        return -ENODEV;
    return (virtio_gpu_present() == 0) ? 0 : -EIO;
}

void sys_fb_register(void) {
    syscall_register(SYS_FB_INFO,  sys_fb_info);
    syscall_register(SYS_FB_MAP,   sys_fb_map);
    syscall_register(SYS_FB_FLUSH, sys_fb_flush);
}
