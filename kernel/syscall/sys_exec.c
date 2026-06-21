/* kernel/syscall/sys_exec.c — sys_execve: replace the calling process image
 * (Phase 5b slice 7, ADR-022).
 *
 * Reads an ELF from the process root filesystem, builds a fresh W^X address
 * space for it (elf_build_image — the same loader core elf_load uses), then
 * atomically replaces the current process: close non-stdio fds, point the TCB
 * at the new address space, tear down the old one, and drop to ring 3 at the new
 * entry via the standard IRETQ helper (enter_user_mode, as in the first launch).
 *
 * Why this is safe mid-syscall: SYSCALL entry clears IF (MSR_SFMASK), so the
 * handler runs without preemption; kernel stacks are mapped in every address
 * space (the scheduler relies on the same fact), so switching CR3 here does not
 * pull the stack out from under us; and both the SYSCALL and interrupt entry
 * paths reset RSP to the thread's kernel-stack top, so abandoning the current
 * syscall frame when we drop to ring 3 leaks nothing.
 *
 * On success it never returns. On any failure BEFORE the commit point the old
 * image keeps running and a negative errno is returned (the new partial address
 * space, if any, is torn down by elf_build_image).
 *
 * Baseline scope (ADR-022): argv[0] is the path; argv/envp marshalling and
 * close-on-exec are deferred. The image must fit the platform's 8 KiB user-ELF
 * budget (matches the Makefile user-ELF cap and the SFS bootstrap buffer);
 * enlarging it is a later slice.
 */
#include "sys_exec.h"
#include "syscall.h"
#include "sched.h"
#include "fd.h"
#include "vfs.h"
#include "elf.h"
#include "vmm.h"
#include "uaccess.h"
#include "kheap.h"
#include "errno.h"
#include <stddef.h>

#define PATH_MAX 256
#define EXEC_MAX 8192u            /* current user-ELF size budget (Makefile cap) */

extern void enter_user_mode(uint64_t rip, uint64_t rsp, uint64_t arg); /* usermode.asm */

static long sys_execve(long upath, long uargv, long uenvp, long a4) {
    (void)uargv; (void)uenvp; (void)a4;        /* baseline: argv[0] = path only */
    struct tcb *t = current_thread;
    if (t->root_mnt < 0)
        return -ENOENT;                        /* no filesystem to exec from */

    char kpath[PATH_MAX];
    ssize_t plen = copyinstr(kpath, (const void __user *)(uintptr_t)upath,
                             sizeof kpath, NULL);
    if (plen < 0)
        return (long)plen;                     /* -EFAULT or -ENAMETOOLONG */

    /* Open + size the target before any address-space work. */
    struct vfs_file vf;
    if (vfs_open(t->fs_cap, t->root_mnt, kpath, &vf) != 0)
        return -ENOENT;
    if (vf.size == 0 || vf.size > EXEC_MAX)
        return -ENOEXEC;

    uint32_t isize = (uint32_t)vf.size;
    uint8_t *buf = (uint8_t *)kmalloc(isize);
    if (!buf)
        return -ENOMEM;
    if (vfs_read(t->fs_cap, &vf, 0, buf, isize) != (int)isize) {
        kfree(buf);
        return -EIO;
    }

    /* Build the new address space. This does NOT touch the current image, so a
     * failure here leaves the caller running unharmed. */
    uint64_t new_as = 0, entry = 0, new_rsp = 0;
    int rc = elf_build_image(buf, isize, kpath, &new_as, &entry, &new_rsp);
    kfree(buf);
    if (rc != ELF_OK)
        return -ENOEXEC;

    /* ---- point of no return: commit the new image (IF is clear here) ---- */
    for (int fd = 3; fd < FD_MAX; fd++)
        fd_free(t, fd);                        /* keep stdio (0/1/2) across exec */

    uint64_t old_as = t->cr3;
    t->cr3        = new_as;
    t->user_rip   = entry;
    t->user_stack = new_rsp;
    for (int i = 0; i < VM_AREA_MAX; i++) { t->vma[i].base = 0; t->vma[i].npages = 0; }
    t->mmap_next  = VMM_MMAP_BASE;             /* anon mmap arena reset for the new image */

    /* Activate the new AS before freeing the old one — never free the live CR3. */
    __asm__ volatile("mov %0, %%cr3" :: "r"(new_as) : "memory");
    if (old_as)
        vmm_destroy_address_space(old_as);

    enter_user_mode(entry, new_rsp, t->user_arg);   /* -> ring 3; never returns */
    return 0;                                  /* unreachable (C3-4) */
}

void sys_exec_register(void) {
    syscall_register(SYS_EXECVE, sys_execve);
}
