/* kernel/syscall/syscall.h — NEXUS syscall interface (NSI), Phase 2e.
 *
 * 64-bit SYSCALL/SYSRET. User mode puts the syscall number in RAX and args in
 * RDI, RSI, RDX, R10 (SysV syscall ABI); the return value comes back in RAX.
 * Mutating syscalls are capability-aware (they validate a capability handle the
 * caller passes).
 */
#pragma once
#include <stdint.h>

/* Syscall numbers (NSI-v2, ADR-022: own dense number space, append-only). 1..4
 * are the Phase 2e bootstrap calls and never move. */
#define SYS_PUTC   1   /* (cap, char)  -> write one char to the console     */
#define SYS_GETPID 2   /* ()           -> current pid                       */
#define SYS_YIELD  3   /* ()           -> yield the CPU                      */
#define SYS_EXIT   4   /* (code)       -> terminate the calling thread      */
#define SYS_READ   5   /* (fd, buf, n)        -> bytes read   (5b slice 3/4) */
#define SYS_WRITE  6   /* (fd, buf, n)        -> bytes written (5b slice 3)  */
#define SYS_OPEN   7   /* (path, flags, mode) -> fd          (5b slice 4)    */
#define SYS_CLOSE  8   /* (fd)                -> 0           (5b slice 4)    */
#define SYS_FSTAT  9   /* (fd, struct stat *) -> 0           (5b slice 4)    */
#define SYS_LSEEK  10  /* (fd, off, whence)   -> new offset  (5b slice 5)    */
#define SYS_GETCWD 11  /* (buf, size)         -> len incl NUL (5b slice 5)   */
#define SYS_MMAP   12  /* (addr, len, prot, flags) -> addr   (5b slice 6)    */
#define SYS_MUNMAP 13  /* (addr, len)         -> 0           (5b slice 6)    */
#define SYS_EXECVE 14  /* (path, argv, envp)  -> no return on success (5b sl 7) */
#define SYS_FORK   15  /* ()  -> child pid in parent, 0 in child  (5b slice 8) */
#define SYS_WAIT4  16  /* (pid, *status, options) -> reaped pid     (5b slice 9) */
#define SYS_PIPE   17  /* (int fds[2])        -> 0; read+write fds  (PROC-A)     */
#define SYS_DUP2   18  /* (oldfd, newfd)      -> newfd              (PROC-A)     */

#define CONSOLE_RES_ID 1ull   /* capability resource id for the console */

typedef long (*syscall_fn)(long a1, long a2, long a3, long a4);

void syscall_init(void);                       /* program MSRs + register table */
void syscall_register(unsigned num, syscall_fn fn);
long syscall_dispatch(long num, long a1, long a2, long a3, long a4);  /* from asm */

/* Kernel stack top for the current user thread's SYSCALL entry (set on switch
 * to a user thread). The asm entry stub reads it; a brief scratch slot too. */
extern uint64_t syscall_kstack_top;
extern uint64_t syscall_user_rsp;
/* User RIP/RSP of the in-flight syscall (captured by syscall_entry.asm). fork
 * uses these as the child's ring-3 resume point. Valid only inside a syscall. */
extern uint64_t syscall_user_rip;
