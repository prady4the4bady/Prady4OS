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
/* Per-client surfaces (Layer 7, DDR-706). Append-only after 47. */
#define SYS_SURFACE_CREATE 48  /* (w, h) -> surface_id | -errno                     */
#define SYS_SURFACE_MAP    49  /* (id) -> user VA | -errno   (owner draws)          */
#define SYS_SURFACE_COMMIT 50  /* (id, x, y) -> 0 | -errno   (mark visible)         */
#define SYS_SURFACE_POLL   51  /* (struct surface_info*, max) -> count (compositor) */
#define SYS_SURFACE_CMAP   52  /* (id) -> user VA  (compositor read-maps a surface) */
/* Named-agent roster (Layer 7, DDR-707). */
#define SYS_AGENT_ROSTER   53  /* (u8 buf*, max) -> count of roster active-bytes     */
/* Surface z-order / focus / input routing (Layer 7, DDR-708). */
#define SYS_SURFACE_RAISE  54  /* (id) -> 0   raise to top + focus                   */
#define SYS_SURFACE_SENDKEY 55 /* (id, ch) -> 0   compositor forwards a key          */
#define SYS_SURFACE_GETKEY 56  /* (id) -> ch | -1   owner drains its key ring        */
/* Wall-clock for time-of-day ambiance (Layer 7, DDR-709). */
#define SYS_CLOCK          57  /* () -> seconds since midnight (RTC)                 */
/* Window move (Layer 7, DDR-710). */
#define SYS_SURFACE_MOVE   58  /* (id, x, y) -> 0  (owner or compositor)             */
/* Window close + resize (Layer 7, DDR-711). */
#define SYS_SURFACE_CLOSE  59  /* (id) -> 0  free the surface (owner or compositor)  */
#define SYS_SURFACE_RESIZE 60  /* (id, w, h) -> 0  new buffer, same pos (owner only) */
/* Window title strings (Layer 7, DDR-715). */
#define SYS_SURFACE_SET_TITLE 61 /* (id, str) -> 0  <=15 chars + NUL (owner only)    */
/* Surface event channel (Layer 7, DDR-718). */
#define SYS_SURFACE_SENDEV 62  /* (id, type, (w<<16)|h) -> 0  compositor/owner push  */
#define SYS_SURFACE_GETEV  63  /* (id, struct surf_event*) -> 0 | -EAGAIN (owner)    */
/* Per-agent live metrics (Layer 7, DDR-730). Append-only after 63. */
#define SYS_AGENT_METRICS  64  /* (struct agent_metric*, max) -> count (observability)*/
/* CAP_NET egress allowlist (DDR-734). */
#define SYS_NET_ALLOW      65  /* (host_be, port) -> 0 | -EPERM | -ENOSPC (sovereign) */
/* Ring-3 directory listing (DDR-742). */
#define SYS_GETDENTS       66  /* (path, index, name_buf) -> namelen | 0(end) | -errno */
/* Ring-3 process listing (DDR-743). */
#define SYS_GETPROCS       67  /* (index, struct procinfo*) -> 1(filled) | 0(end) | -errno */
/* Ring-3 file removal (DDR-744; O_CREAT-on-open handled in sys_open flags). */
#define SYS_UNLINK         68  /* (path) -> 0 | -errno  (file or empty dir)             */
/* ACPI S5 poweroff (DDR-746; CAP_SOVEREIGN). No return on success. */
#define SYS_POWEROFF       69  /* () -> -EPERM (non-sovereign) | -ENODEV (no S5)        */
/* ACPI/PC reboot (DDR-747; CAP_SOVEREIGN). No return on success. */
#define SYS_REBOOT         70  /* () -> -EPERM (non-sovereign)                          */
/* System/CPU introspection (DDR-748; no cap — read-only). */
#define SYS_SYSINFO        71  /* (struct sysinfo*) -> 0 | -EFAULT                      */
/* Wall-clock date/time (DDR-749; no cap — read-only RTC). */
#define SYS_TIME           72  /* (struct rtc_time*) -> 0 | -EFAULT                     */
/* Kernel log read-back (DDR-750; no cap — diagnostic). */
#define SYS_DMESG          73  /* (buf, max) -> bytes copied | -EFAULT                  */
/* Physical memory accounting (DDR-752; no cap — read-only). */
#define SYS_MEMINFO        74  /* (struct meminfo*) -> 0 | -EFAULT                      */

#define CONSOLE_RES_ID 1ull   /* capability resource id for the console */

typedef long (*syscall_fn)(long a1, long a2, long a3, long a4);

void syscall_init(void);                       /* program MSRs + register table */
void syscall_init_ap(void);                    /* cap-4: arm THIS CPU's SYSCALL MSRs */
void syscall_register(unsigned num, syscall_fn fn);
long syscall_dispatch(long num, long a1, long a2, long a3, long a4);  /* from asm */

/* The kernel stack top for SYSCALL entry lives in the percpu area at [gs:16]
 * (DDR-SMP-3b). The in-flight syscall's user-register snapshot (RSP/RIP,
 * callee-saved regs, RFLAGS — fork's child resume state) is per-CPU too:
 * `this_cpu()->u_*` (percpu.h), written by syscall_entry.asm (ADR-031 cap-4).
 * Valid only inside a syscall, on the syscall's own CPU. */
