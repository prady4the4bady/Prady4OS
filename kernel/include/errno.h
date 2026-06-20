/* kernel/include/errno.h — POSIX errno values (Phase 5b, ADR-022).
 *
 * NSI-v2 convention: a syscall handler (and the uaccess primitives) return the
 * NEGATED value on failure — e.g. `return -EFAULT;`. A success is >= 0. The musl
 * arch port (slice 5c) negates the returned value back into `errno`.
 */
#pragma once

#define EPERM           1   /* operation not permitted              */
#define ENOENT          2   /* no such file or directory            */
#define EBADF           9   /* bad file descriptor                  */
#define ECHILD         10   /* no child processes                   */
#define ENOMEM         12   /* out of memory                        */
#define EACCES         13   /* permission denied                    */
#define EFAULT         14   /* bad address (user pointer)           */
#define EINVAL         22   /* invalid argument                     */
#define EMFILE         24   /* too many open files                  */
#define ESPIPE         29   /* illegal seek (non-seekable fd)       */
#define ERANGE         34   /* result too large for the buffer      */
#define ENAMETOOLONG   36   /* file name / string too long          */
#define ENOSYS         38   /* function not implemented             */
