/* kernel/proc/fd.h — per-process file-descriptor table (Phase 5b, ADR-022).
 * ===========================================================================
 * Each process (TCB) carries a fixed FD_MAX-entry table. fds 0/1/2 are pre-wired
 * to the kernel console (stdin/stdout/stderr); higher fds are bound to VFS files
 * by sys_open (slice 4). The capability that authorized an fd is stored in the
 * entry, so NCS enforcement survives under the POSIX fd API: open checks the
 * process FS capability, and read/write/lseek/fstat use the stored handle.
 *
 * The entry holds a *pointer* to the heap-allocated vfs_file (not the struct by
 * value) so this header — pulled into the widely-included sched.h — stays light
 * and does not drag vfs.h across the tree.
 * ===========================================================================
 */
#pragma once
#include <stdint.h>
#include "cap.h"

struct vfs_file;       /* forward decl: entries hold a kmalloc'd pointer */
struct tcb;            /* forward decl: fd ops take the owning thread     */
struct pipe;           /* forward decl: FD_PIPE entries reference a pipe  */
struct epoll;          /* forward decl: FD_EPOLL entries reference one    */

#define FD_MAX 64

enum fd_kind {
    FD_NONE = 0,       /* free slot                                       */
    FD_CONSOLE,        /* stdin/stdout/stderr -> kernel console           */
    FD_VFS,            /* regular file backed by the VFS (slice 4)        */
    FD_PIPE,           /* anonymous pipe end (PROC-A)                     */
    FD_EPOLL,          /* epoll instance (PROC-B)                         */
};

struct fd_entry {
    int              kind;     /* enum fd_kind                            */
    uint64_t         off;      /* current byte offset (FD_VFS)            */
    int              mnt;      /* owning mount id (FD_VFS)                */
    struct vfs_file *file;     /* heap-allocated open-file state (FD_VFS) */
    struct pipe     *pipe;     /* shared pipe object (FD_PIPE)            */
    struct epoll    *epoll;    /* epoll instance (FD_EPOLL)               */
    cap_t            cap;      /* capability authorizing this fd          */
    int              flags;    /* open flags (FD_VFS) / PIPE_WRITE_END    */
};

struct fd_table {
    struct fd_entry e[FD_MAX];
};

/* Initialize a table to all-free (called for every thread). */
void fd_table_init(struct fd_table *t);

/* Pre-wire fds 0/1/2 to the console (called for user threads). */
void fd_init_std(struct fd_table *t);

/* Lowest free fd index, or -1 if the table is full. */
int  fd_alloc(struct tcb *t);

/* Entry for `fd` if it is in range and in use, else NULL. */
struct fd_entry *fd_get(struct tcb *t, int fd);

/* Release an fd, freeing any heap-allocated vfs_file. */
void fd_free(struct tcb *t, int fd);

/* fork: deep-copy every fd entry from `src` to `dst`. Each FD_VFS entry gets its
 * OWN heap vfs_file copy (no shared pointer → no double-close), so parent and
 * child can close independently. The seek offset is copied, not shared (a
 * documented divergence from POSIX shared open-file descriptions, acceptable for
 * the short-lived test processes). Returns 0 on success, -ENOMEM on alloc fail
 * (any partial copies in `dst` are freed before returning). */
int fd_clone(struct tcb *src, struct tcb *dst);
