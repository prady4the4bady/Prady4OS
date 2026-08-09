/* kernel/syscall/sys_vault.c — credential vault ring-3 interface (DDR-834).
 *
 * BOTH calls are CAP_SOVEREIGN. A CAP_AGENT get was considered — the cloud
 * bridge is the consumer and agents run the bridge — and rejected: handing
 * plaintext credentials to any CAP_AGENT caller means one compromised agent
 * drains the vault, which is exactly the blast radius the vault exists to bound.
 *
 * The vault protects credentials AT REST and against a compromised agent. It
 * cannot protect them from a compromised sovereign, and does not pretend to.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "cap.h"
#include "aether.h"
#include "vault.h"

/* The owner seed is NOT a ring-3 parameter. If the caller supplied it, a
 * sovereign process could read any record by presenting its own seed, and the
 * vault would be a decryption oracle rather than a vault. */
static const uint8_t g_owner_seed[32] = {
    0x5A, 0x11, 0x9C, 0x2E, 0x74, 0x03, 0xB8, 0xD6,
    0x41, 0xEF, 0x27, 0x90, 0xAC, 0x1B, 0x63, 0xF5,
    0x08, 0xD2, 0x4E, 0x86, 0x39, 0xCB, 0x7A, 0x15,
    0x62, 0xF0, 0x9D, 0x34, 0xE7, 0x58, 0x2C, 0xB1
};

/* ring-3 ABI: (name, secret, secretlen, 0) */
static long sys_vault_put(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t slen = (uint32_t)a3;
    if (slen == 0 || slen > VAULT_MAX_SECRET)
        return -EINVAL;

    char name[VAULT_MAX_NAME + 1];
    uint8_t secret[VAULT_MAX_SECRET];
    uint64_t nlen = 0;
    if (copyinstr(name, (const void __user *)a1, sizeof name, &nlen) < 0)
        return -EFAULT;
    if (copyin(secret, (const void __user *)a2, slen) < 0)
        return -EFAULT;

    int r = vault_put(current_thread->fs_cap, current_thread->root_mnt,
                      name, secret, slen, g_owner_seed);
    if (r != VAULT_OK) {
        aether_audit(current_thread->pid, 0, (uint64_t)(-r), AR_VAULT_REJECTED);
        return (r == VAULT_ERR_NOSPC) ? -ENOSPC
             : (r == VAULT_ERR_ARG)   ? -EINVAL
             : (r == VAULT_ERR_RNG)   ? -EAGAIN
             : (r == VAULT_ERR_KEY)   ? -ENOKEY
                                      : -EIO;
    }
    aether_audit(current_thread->pid, 0, slen, AR_VAULT_PUT);
    return 0;
}

/* ring-3 ABI: (name, out, outlen_ptr, 0) */
static long sys_vault_get(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }

    char name[VAULT_MAX_NAME + 1];
    uint64_t nlen = 0;
    if (copyinstr(name, (const void __user *)a1, sizeof name, &nlen) < 0)
        return -EFAULT;

    uint8_t out[VAULT_MAX_SECRET];
    uint32_t outlen = 0;
    int r = vault_get(current_thread->fs_cap, current_thread->root_mnt,
                      name, out, &outlen, g_owner_seed);
    if (r != VAULT_OK) {
        /* A tampered record and a missing one are DIFFERENT events: the first is
         * an attack on the disk, the second is a caller asking for something
         * that was never stored. Collapsing them would hide the attack. */
        if (r == VAULT_ERR_AUTH)
            aether_audit(current_thread->pid, 0, 0, AR_VAULT_REJECTED);
        return (r == VAULT_ERR_NOENT) ? -ENOENT
             : (r == VAULT_ERR_AUTH)  ? -EACCES
             : (r == VAULT_ERR_ARG)   ? -EINVAL
                                      : -EIO;
    }

    if (copyout((void __user *)a2, out, outlen) < 0)
        return -EFAULT;
    if (copyout((void __user *)a3, &outlen, sizeof outlen) < 0)
        return -EFAULT;
    aether_audit(current_thread->pid, 0, outlen, AR_VAULT_GET);
    return 0;
}

void sys_vault_register(void) {
    syscall_register(SYS_VAULT_PUT, sys_vault_put);   /* NSI 87 */
    syscall_register(SYS_VAULT_GET, sys_vault_get);   /* NSI 91 */
}
