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
#include "mm.h"         /* struct mm, mm->lock (DDR-1190 A1.10) */
#include "swap.h"       /* swap_prefault_locked */

static inline uint64_t cur_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* ---- DDR-1190 A1.10 point 2: the user-copy pin ----------------------------
 * Swap-in, the range check and the pin are one step under the mm lock, so no
 * reclaim can evict a page between the check and the copy: reclaim skips an mm
 * whose uaccess_active is nonzero. The copy itself runs after the lock is
 * dropped, exactly as before; a racing munmap still frees the frames
 * (PTE_SW_DYING -> shootdown -> free, A1.10 point 5) and the copy then takes
 * the uaccess_copy fixup and returns -EFAULT.
 *
 * A thread with no mm (a kernel thread) has nothing to swap and no reclaim to
 * hold off; it keeps the plain range check. A caller that already holds its
 * own mm lock (kmutex is not recursive) pins under that hold. */
int uaccess_pin(const void __user *u, size_t n, int writable) {
    struct tcb *t  = current_thread;
    struct mm  *mm = t ? t->mm : 0;
    uint64_t    va = (uint64_t)(uintptr_t)u;
    int held = mm && mm->lock.owner == t;
    if (mm && !held)
        kmutex_lock(&mm->lock);
    int ok = n == 0 ||
             swap_user_range_ok_locked(mm, cur_cr3(), va, (uint64_t)n, writable);
    if (ok && t) {
        if (mm)
            __atomic_add_fetch(&mm->uaccess_active, 1, __ATOMIC_ACQ_REL);
        t->uaccess_pins++;
    }
    if (mm && !held)
        kmutex_unlock(&mm->lock);
    return ok ? 0 : -EFAULT;
}

void uaccess_unpin(void) {
    struct tcb *t = current_thread;
    if (!t || !t->uaccess_pins)
        return;
    t->uaccess_pins--;
    if (t->mm)
        __atomic_sub_fetch(&t->mm->uaccess_active, 1, __ATOMIC_ACQ_REL);
}

/* A1.10 point 3: the copies a caller makes under a spinlock, after pinning
 * before it took the lock. No lock, no sleep: without this thread's own pin,
 * or with any page of the range not present (a swapped page is not present),
 * they refuse. */
static int pinned_ok(uint64_t va, uint64_t n, int writable) {
    struct tcb *t = current_thread;
    return t && t->uaccess_pins && vmm_user_range_ok(cur_cr3(), va, n, writable);
}

ssize_t copyin_pinned(void *kdst, const void __user *usrc, size_t n) {
    if (n == 0)
        return 0;
    if (!pinned_ok((uint64_t)(uintptr_t)usrc, (uint64_t)n, 0))
        return -EFAULT;
    uaccess_begin();                    /* DDR-1041 */
    long rc = uaccess_copy(kdst, (const void *)usrc, n);   /* DDR-1181 §3.7 */
    uaccess_end();
    return rc ? rc : (ssize_t)n;
}

ssize_t copyout_pinned(void __user *udst, const void *ksrc, size_t n) {
    if (n == 0)
        return 0;
    if (!pinned_ok((uint64_t)(uintptr_t)udst, (uint64_t)n, 1))
        return -EFAULT;
    uaccess_begin();                    /* DDR-1041 */
    long rc = uaccess_copy((void *)udst, ksrc, n);         /* DDR-1181 §3.7 */
    uaccess_end();
    return rc ? rc : (ssize_t)n;
}

ssize_t copyin(void *kdst, const void __user *usrc, size_t n) {
    if (n == 0)
        return 0;
    if (uaccess_pin(usrc, n, 0) < 0)    /* DDR-1190 A1.10: swap-in + check */
        return -EFAULT;
    uaccess_begin();                    /* DDR-1041: SMAP window opens HERE --
                                         * after validation, so a bad pointer is
                                         * still rejected with AC clear. */
    long rc = uaccess_copy(kdst, (const void *)usrc, n);   /* DDR-1181 §3.7 */
    uaccess_end();
    uaccess_unpin();
    return rc ? rc : (ssize_t)n;
}

ssize_t copyout(void __user *udst, const void *ksrc, size_t n) {
    if (n == 0)
        return 0;
    /* writable=1: the destination pages must be present, user, AND RW — a write
     * to a read-only / text user page is rejected here (W^X upheld). */
    if (uaccess_pin(udst, n, 1) < 0)    /* DDR-1190 A1.10 */
        return -EFAULT;
    uaccess_begin();                    /* DDR-1041 */
    long rc = uaccess_copy((void *)udst, ksrc, n);         /* DDR-1181 §3.7 */
    uaccess_end();
    uaccess_unpin();
    return rc ? rc : (ssize_t)n;
}

ssize_t copyinstr(char *kdst, const void __user *usrc, size_t max, size_t *lenout) {
    if (lenout)
        *lenout = 0;
    if (max == 0)
        return -ENAMETOOLONG;

    uint64_t ua  = (uint64_t)(uintptr_t)usrc;
    int pinned = 0;                     /* DDR-1190 A1.10: one pin, per page */

    for (size_t i = 0; i < max; i++) {
        uint64_t va = ua + i;
        /* Validate each page as we first step into it (i == 0, or a page
         * boundary crossing). Cheap: one walk per 4 KiB, not per byte. The
         * pin swaps the page in and holds reclaim off it while it is read. */
        if (i == 0 || (va & (PAGE_SIZE - 1)) == 0) {
            if (pinned)
                uaccess_unpin();
            pinned = uaccess_pin((const void __user *)(uintptr_t)va, 1, 0) == 0;
            if (!pinned)
                return -EFAULT;
        }
        /* DDR-1041: the window is opened and closed around the ONE byte, not
         * hoisted out of the loop. Hoisting would leave AC set across the
         * vmm_user_range_ok walk at every page boundary — i.e. across kernel
         * page-table reads — which is precisely the exposure SMAP exists to
         * remove. One stac/clac pair per byte is the honest cost of a
         * byte-at-a-time reader; a bulk reader would pay it once per page. */
        char c;
        uaccess_begin();
        long rc = uaccess_copy(&c, (const void *)(uintptr_t)va, 1);  /* DDR-1181 §3.7 */
        uaccess_end();
        if (rc) {
            uaccess_unpin();
            return rc;
        }
        kdst[i] = c;
        if (c == '\0') {
            uaccess_unpin();
            if (lenout)
                *lenout = i;            /* length excluding the terminator */
            return (ssize_t)i;
        }
    }
    if (pinned)
        uaccess_unpin();
    return -ENAMETOOLONG;               /* no NUL within `max` bytes */
}
