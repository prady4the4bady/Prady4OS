/* kernel/syscall/sys_fb.c — ring-3 framebuffer surface NSI (Layer 7, DDR-702).
 *
 * Three handlers over the active display (DDR-1142: virtio-gpu, ADR-028, or
 * the UEFI GOP framebuffer the loader handed over): query geometry, map the
 * front buffer into the caller for direct drawing, and present it. With no
 * display up, all three return -ENODEV so a ring-3 program degrades cleanly.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "vmm.h"
#include "pmm.h"          /* PAGE_SIZE */
#include "display.h"    /* DDR-1142: virtio-gpu or GOP */

#define FB_USER_VA 0x8700000000ull   /* below the mmap arena (VMM_MMAP_BASE) */

struct fb_info { uint32_t width, height, stride, bpp; };

static long sys_fb_info(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    uint32_t w, h, stride;
    if (!display_fb(&w, &h, &stride, 0))
        return -ENODEV;
    struct fb_info fi = { w, h, stride, 32 };
    if (copyout((void __user *)a1, &fi, sizeof fi) < 0)
        return -EFAULT;
    return 0;
}

static long sys_fb_map(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a1; (void)a2; (void)a3; (void)a4;
    uint32_t w, h, stride;
    uint64_t fb_phys;
    if (!display_fb(&w, &h, &stride, &fb_phys))
        return -ENODEV;
    struct tcb *t = current_thread;
    /* DDR-1142: map the PHYSICAL address the backend reports. This used to be
     * `phys = (uintptr_t)fb` ("identity-mapped: phys == kvirt"), true of
     * virtio-gpu's PMM buffer and false of a GOP framebuffer, which is device
     * memory outside the identity map. A GOP base need not be page-aligned, so
     * the in-page offset is carried into the returned VA. */
    uint64_t off  = fb_phys & (PAGE_SIZE - 1);
    uint64_t phys = fb_phys - off;
    uint64_t bytes = off + (uint64_t)stride * h;
    uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    /* DDR-729: PTE_SW_SHARED marks this a VIEW of the GPU-owned scanout frames, so
     * vmm_destroy_address_space (free_subtree) never frees them when a client that
     * mapped the FB exits — the GPU resource is not the client's private memory. */
    uint64_t flags = VMM_USER | VMM_RW | VMM_NX | PTE_SW_SHARED;  /* data surface; never executable */
    for (uint64_t i = 0; i < npages; i++) {
        if (vmm_map_in(t->cr3, FB_USER_VA + i * PAGE_SIZE,
                       phys + i * PAGE_SIZE, flags) != 0)
            return -ENOMEM;                       /* partial map: caller won't use it */
    }
    return (long)(FB_USER_VA + off);
}

static long sys_fb_flush(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (!display_fb(0, 0, 0, 0))
        return -ENODEV;
    return (display_present() == 0) ? 0 : -EIO;
}

void sys_fb_register(void) {
    syscall_register(SYS_FB_INFO,  sys_fb_info);
    syscall_register(SYS_FB_MAP,   sys_fb_map);
    syscall_register(SYS_FB_FLUSH, sys_fb_flush);
}
