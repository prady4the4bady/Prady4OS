/* kernel/syscall/sys_agentmem.c — agent memory ring-3 interface (DDR-836).
 *
 * Both calls require CAP_MEMORY. Reads are audited as well as writes: on a
 * SHARED blackboard (see agentmem.h) "who read this fact" is as much an operator
 * question as who wrote it, and auditing only writes would leave half the
 * information flow invisible.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "cap.h"
#include "aether.h"
#include "agentmem.h"

/* ring-3 ABI: (key, val, vallen) */
static long sys_memory_write(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    if (!current_thread->is_memory && !current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t vlen = (uint32_t)a3;
    if (vlen == 0 || vlen > MEM_MAX_VAL)
        return -EINVAL;

    char key[MEM_MAX_KEY + 1];
    uint8_t val[MEM_MAX_VAL];
    uint64_t klen = 0;
    if (copyinstr(key, (const void __user *)a1, sizeof key, &klen) < 0)
        return -EFAULT;
    if (copyin(val, (const void __user *)a2, vlen) < 0)
        return -EFAULT;

    int r = agentmem_write(key, val, vlen);
    if (r != MEM_OK)
        return (r == MEM_ERR_NOSPC) ? -ENOSPC : -EINVAL;

    aether_audit(current_thread->pid, 0, vlen, AR_MEM_WRITE);
    return 0;
}

/* ring-3 ABI: (key, out, outlen_ptr) */
static long sys_memory_read(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    if (!current_thread->is_memory && !current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }

    char key[MEM_MAX_KEY + 1];
    uint64_t klen = 0;
    if (copyinstr(key, (const void __user *)a1, sizeof key, &klen) < 0)
        return -EFAULT;

    uint8_t out[MEM_MAX_VAL];
    uint32_t outlen = 0;
    int r = agentmem_read(key, out, &outlen);
    if (r != MEM_OK)
        return (r == MEM_ERR_NOENT) ? -ENOENT : -EINVAL;

    if (copyout((void __user *)a2, out, outlen) < 0)
        return -EFAULT;
    if (copyout((void __user *)a3, &outlen, sizeof outlen) < 0)
        return -EFAULT;

    aether_audit(current_thread->pid, 0, outlen, AR_MEM_READ);
    return 0;
}

void sys_agentmem_register(void) {
    syscall_register(SYS_MEMORY_WRITE, sys_memory_write);  /* NSI 82 */
    syscall_register(SYS_MEMORY_READ,  sys_memory_read);   /* NSI 83 */
}
