/* kernel/mm/uaccess.c — validated user<->kernel copy primitives (Phase 5b).
 *
 * Implements the ADR-022 user-pointer contract on top of vmm_user_range_ok,
 * which walks the calling process's page tables WITHOUT dereferencing the user
 * address — so a bad pointer is reported as -EFAULT and the kernel never #PFs at
 * CPL 0. During a syscall the active CR3 is the caller's process AS, so we read
 * it directly with `mov %cr3`.
 */
#include "uaccess.h"
#include "vmm.h"
#include "pmm.h"        /* PAGE_SIZE */
#include "errno.h"
#include "string.h"

static inline uint64_t cur_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

ssize_t copyin(void *kdst, const void __user *usrc, size_t n) {
    if (n == 0)
        return 0;
    if (!vmm_user_range_ok(cur_cr3(), (uint64_t)(uintptr_t)usrc, (uint64_t)n, 0))
        return -EFAULT;
    memcpy(kdst, (const void *)usrc, n);
    return (ssize_t)n;
}

ssize_t copyout(void __user *udst, const void *ksrc, size_t n) {
    if (n == 0)
        return 0;
    /* writable=1: the destination pages must be present, user, AND RW — a write
     * to a read-only / text user page is rejected here (W^X upheld). */
    if (!vmm_user_range_ok(cur_cr3(), (uint64_t)(uintptr_t)udst, (uint64_t)n, 1))
        return -EFAULT;
    memcpy((void *)udst, ksrc, n);
    return (ssize_t)n;
}

ssize_t copyinstr(char *kdst, const void __user *usrc, size_t max, size_t *lenout) {
    if (lenout)
        *lenout = 0;
    if (max == 0)
        return -ENAMETOOLONG;

    uint64_t cr3 = cur_cr3();
    uint64_t ua  = (uint64_t)(uintptr_t)usrc;

    for (size_t i = 0; i < max; i++) {
        uint64_t va = ua + i;
        /* Validate each page as we first step into it (i == 0, or a page
         * boundary crossing). Cheap: one walk per 4 KiB, not per byte. */
        if (i == 0 || (va & (PAGE_SIZE - 1)) == 0) {
            if (!vmm_user_range_ok(cr3, va, 1, 0))
                return -EFAULT;
        }
        char c = *(const volatile char *)(uintptr_t)va;
        kdst[i] = c;
        if (c == '\0') {
            if (lenout)
                *lenout = i;            /* length excluding the terminator */
            return (ssize_t)i;
        }
    }
    return -ENAMETOOLONG;               /* no NUL within `max` bytes */
}
