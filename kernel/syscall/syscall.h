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
#define SYS_EPOLL_CREATE 19 /* (size)        -> epoll fd            (PROC-B)     */
#define SYS_EPOLL_CTL    20 /* (epfd, op, fd, *ev) -> 0            (PROC-B)     */
#define SYS_EPOLL_WAIT   21 /* (epfd, *evs, max, timeout) -> nready (PROC-B)    */
#define SYS_SIGACTION    22 /* (signum, handler_va) -> 0            (PROC-C)     */
#define SYS_KILL         23 /* (pid, signum)        -> 0            (PROC-C)     */
#define SYS_SIGRETURN    24 /* () -> no return (restores frame)    (PROC-C)     */
#define SYS_IO_URING_SETUP 25 /* (entries)        -> ring user VA   (PROC-E)     */
#define SYS_IO_URING_ENTER 26 /* (ring_va, to_submit) -> n done    (PROC-E)     */
#define SYS_SET_TLS  27  /* (fs_base) -> 0; set the thread pointer  (PROC-D, ADR-023) */
#define SYS_WRITEV   28  /* (fd, iov, iovcnt) -> bytes written     (PROC-D, ADR-023) */
/* AETHER agent layer (Layer 6, ADR-026 §D6 / DDR §1). Append-only after 28. */
#define SYS_GET_MODE       29  /* () -> mode (0|1)                                  */
#define SYS_SET_MODE       30  /* (mode) -> 0 | -EPERM       (needs CAP_SOVEREIGN)  */
#define SYS_SUBMIT_ACTION  31  /* (type, payload*, len) -> action_id | -EAGAIN      */
#define SYS_POLL_RESULT    32  /* (action_id) -> status | -ESRCH                    */
#define SYS_APPROVE_ACTION 33  /* (action_id) -> 0 | -EPERM  (CAP_SOVEREIGN)        */
#define SYS_REJECT_ACTION  34  /* (action_id) -> 0 | -EPERM  (CAP_SOVEREIGN)        */
#define SYS_SPAWN_AGENT    35  /* (path*, task*) -> pid | -EPERM   (CAP_AGENT)      */
#define SYS_KILL_AGENT     36  /* (pid) -> 0 | -EPERM|-ESRCH       (CAP_AGENT)      */
#define SYS_READ_AUDIT     37  /* (buf*, max) -> n entries copied                   */
#define SYS_SET_MEM_LIMIT  38  /* (pid, bytes) -> 0 | -EPERM (lower-only)           */
/* Ring-3 proxy sockets (ADR-027). Append-only after 38. */
#define SYS_SOCK_CONNECT   39  /* (host_be, port) -> fd(0..7) | -errno              */
#define SYS_SOCK_WRITE     40  /* (fd, buf, len) -> bytes written | -errno          */
#define SYS_SOCK_READ      41  /* (fd, buf, len, timeout_ms) -> bytes | 0(EOF) | -errno */
#define SYS_SOCK_CLOSE     42  /* (fd) -> 0 | -errno                                */
/* Ring-3 framebuffer surface (Layer 7, DDR-702). Append-only after 42. */
#define SYS_FB_INFO        43  /* (struct fb_info*) -> 0 | -ENODEV                  */
#define SYS_FB_MAP         44  /* () -> user VA | -ENODEV                           */
#define SYS_FB_FLUSH       45  /* () -> 0 | -ENODEV  (present the framebuffer)      */
#define SYS_INPUT_POLL     46  /* (buf, max) -> count of keyboard bytes (DDR-703)   */
#define SYS_MOUSE_POLL     47  /* (struct mouse_state*) -> 0 | -ENODEV (DDR-705)    */

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
/* Parent's callee-saved regs + RFLAGS captured at SYSCALL entry, used by fork to
 * resume the child with a full register set (RAX=0). Valid only inside a syscall. */
extern uint64_t syscall_user_rbx, syscall_user_rbp;
extern uint64_t syscall_user_r12, syscall_user_r13, syscall_user_r14, syscall_user_r15;
extern uint64_t syscall_user_rflags;
