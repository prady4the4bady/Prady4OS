/* user/include/pradyos.h — the formal PRADYOS extension API for musl userspace.
 * Group 6 item 32, DDR-869.
 *
 * WHAT THIS IS FOR. musl gives ring-3 the POSIX surface. PRADYOS adds ~50 calls
 * on top — agents, surfaces, the vault, the audit log, mode switching — and
 * until now every ring-3 program reached them by re-declaring `SYS_*` numbers
 * and its own inline `syscall` stub. Around forty probes carry that boilerplate,
 * each an independent chance to mistype a number.
 *
 * THE NUMBERS ARE NOT DUPLICATED HERE. `pradyos_nsi.h` is GENERATED at build
 * time from kernel/syscall/syscall.h, which stays the single source of truth. A
 * hand-copied table in a header is the exact defect this project has hit
 * repeatedly (DDR-817/822/825/833/835): a second list that drifts silently and
 * whose drift looks like working code. If the kernel renumbers a call, this
 * header follows on the next build or the build fails — it cannot quietly
 * disagree.
 *
 * WHAT THIS IS NOT. It is not a libc, and it does not wrap POSIX calls musl
 * already provides. `open`, `read`, `write` and friends come from musl; adding
 * competing wrappers here would give two ways to do the same thing that differ
 * in errno handling.
 *
 * ERRNO CONVENTION. These wrappers return the RAW kernel value: >= 0 on
 * success, negative errno on failure. They deliberately do NOT set errno and
 * return -1 the way musl does. Ring-3 probes assert on exact values
 * (`== -EINVAL`), and a wrapper that collapsed every failure to -1 would make
 * those assertions impossible — which is precisely how DDR-867's negative-length
 * check went untested.
 */
#ifndef PRADYOS_H
#define PRADYOS_H

#include <stdint.h>
#include "pradyos_nsi.h"     /* GENERATED from kernel/syscall/syscall.h */

/* The NSI trap. Args land in RDI/RSI/RDX per the SysV syscall ABI; RCX and R11
 * are clobbered by SYSCALL itself, and "memory" stops the compiler caching
 * across a call that can touch anything. */
static inline long pradyos_syscall(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---- filesystem extensions ------------------------------------------- */

/* Set an open file's length: grows with zeros, shrinks by discarding the tail.
 * Returns 0, or -EINVAL / -EBADF / -EIO / -EPERM (DDR-867). */
static inline long pradyos_ftruncate(int fd, long len) {
    return pradyos_syscall(SYS_FTRUNCATE, fd, len, 0);
}

/* ---- mode ------------------------------------------------------------- */

/* 1 = SOVEREIGN, 0 = MANUAL. */
static inline long pradyos_get_mode(void) {
    return pradyos_syscall(SYS_GET_MODE, 0, 0, 0);
}

/* Requires CAP_SOVEREIGN; -EPERM otherwise. The refusal is the point: an
 * unprivileged caller must not be able to flip the approval gate. */
static inline long pradyos_set_mode(int sovereign) {
    return pradyos_syscall(SYS_SET_MODE, sovereign, 0, 0);
}

/* ---- agent / AETHER --------------------------------------------------- */

static inline long pradyos_submit_action(long type, const void *payload, long len) {
    return pradyos_syscall(SYS_SUBMIT_ACTION, type, (long)payload, len);
}

static inline long pradyos_poll_result(long action_id) {
    return pradyos_syscall(SYS_POLL_RESULT, action_id, 0, 0);
}

/* CAP_SOVEREIGN. An agent approving its own action is the S4 failure, and the
 * kernel refuses it — this wrapper does not soften that. */
static inline long pradyos_approve_action(long action_id) {
    return pradyos_syscall(SYS_APPROVE_ACTION, action_id, 0, 0);
}

static inline long pradyos_reject_action(long action_id) {
    return pradyos_syscall(SYS_REJECT_ACTION, action_id, 0, 0);
}

/* Copies up to `max` audit entries. The log is SHA-256 chained (DDR-842) and
 * has no user-space erase path by construction. */
static inline long pradyos_read_audit(void *buf, long max) {
    return pradyos_syscall(SYS_READ_AUDIT, (long)buf, max, 0);
}

/* 0 = chain intact; -EACCES = broken, with the bad index written out. */
static inline long pradyos_verify_audit(unsigned *bad_index) {
    return pradyos_syscall(SYS_VERIFY_AUDIT, (long)bad_index, 0, 0);
}

/* ---- system introspection --------------------------------------------- */

static inline long pradyos_sysinfo(void *out) {
    return pradyos_syscall(SYS_SYSINFO, (long)out, 0, 0);
}

static inline long pradyos_getprocs(long index, void *out) {
    return pradyos_syscall(SYS_GETPROCS, index, (long)out, 0);
}

/* Directory enumeration: returns the name length, 0 at end, or -errno. */
static inline long pradyos_getdents(const char *path, long index, char *name_buf) {
    return pradyos_syscall(SYS_GETDENTS, (long)path, index, (long)name_buf);
}

#endif /* PRADYOS_H */
