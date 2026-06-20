/* kernel/mm/uaccess.h — validated user<->kernel memory copy (Phase 5b, ADR-022).
 * ===========================================================================
 * Every byte that crosses the ring boundary goes through these three primitives.
 * The kernel NEVER dereferences a raw user pointer anywhere else — a bare
 * `*user_ptr` / memcpy from a user address in a syscall handler is a defect.
 *
 * The contract (binding, equal in standing to W^X — see ADR-022):
 *   - the user range is validated against the calling process's page tables
 *     BEFORE any dereference, so the kernel never takes a #PF at CPL 0 on a
 *     user-supplied address;
 *   - a bad pointer returns -EFAULT (the kernel keeps running), never a panic;
 *   - copyout additionally requires the target pages to be user-WRITABLE, so a
 *     write to a read-only / text user page is -EFAULT, never an honored write
 *     (this is also how W^X is upheld on the copy path).
 *
 * User pointers are tagged `__user` purely as a grep marker (expands to nothing).
 * ===========================================================================
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef __user
#define __user            /* marker: this pointer is an untrusted user VA */
#endif

typedef long ssize_t;

/* Copy `n` bytes user->kernel. Returns n on success, -EFAULT on a bad source. */
ssize_t copyin(void *kdst, const void __user *usrc, size_t n);

/* Copy `n` bytes kernel->user. Returns n on success, -EFAULT if the destination
 * is not present+user+writable for the whole range. */
ssize_t copyout(void __user *udst, const void *ksrc, size_t n);

/* Copy a NUL-terminated string user->kernel, at most `max` bytes incl. the NUL.
 * On success returns the string length (excluding NUL) and, if `lenout` != NULL,
 * stores that length; returns -EFAULT on a bad page, -ENAMETOOLONG if no NUL is
 * found within `max` bytes. */
ssize_t copyinstr(char *kdst, const void __user *usrc, size_t max, size_t *lenout);
