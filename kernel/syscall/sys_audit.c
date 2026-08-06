/* kernel/syscall/sys_audit.c — audit chain verification (DDR-842, NSI 93).
 *
 * VERIFY ONLY. Reading records is the shipped SYS_READ_AUDIT (37), whose entry
 * layout `user/egressaudittest.c`, `user/privacynettest.c` and
 * `user/sovegresstest.c` each carry their own copy of. Widening that struct to
 * carry the chain value would have made the kernel write 32 extra bytes per
 * entry into buffers those probes sized for the old shape — an overflow, not a
 * parse error. So the verdict gets its own call and the wire format of 37 is
 * untouched.
 *
 * CAP_SOVEREIGN: "is the audit log intact?" is an operator question, and an
 * agent that could ask it could also discover exactly when its tampering was
 * noticed.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "cap.h"
#include "aether.h"

/* ring-3 ABI: (bad_index_ptr)
 *   0        chain intact across the retained window
 *   -EACCES  chain BROKEN; *bad_index_ptr receives the first bad index
 *   -EPERM   not sovereign
 *
 * The verdict rides on the RETURN VALUE, not on an out-parameter the caller may
 * forget to read. A tampered log that returned 0 with a quiet flag would be read
 * as success by every caller that only checks for a negative return. */
static long sys_verify_audit(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }

    uint32_t bad = 0;
    if (aether_audit_verify(&bad) != 0) {
        if (a1 && copyout((void __user *)a1, &bad, sizeof bad) < 0)
            return -EFAULT;
        aether_audit(current_thread->pid, 0, (uint64_t)bad, AR_AUDIT_TAMPERED);
        return -EACCES;
    }

    aether_audit(current_thread->pid, 0, 0, AR_AUDIT_READ);
    return 0;
}

void sys_audit_register(void) {
    syscall_register(SYS_VERIFY_AUDIT, sys_verify_audit);   /* NSI 93 */
}
