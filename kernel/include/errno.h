/* kernel/include/errno.h — POSIX errno values (Phase 5b, ADR-022).
 *
 * NSI-v2 convention: a syscall handler (and the uaccess primitives) return the
 * NEGATED value on failure — e.g. `return -EFAULT;`. A success is >= 0. The musl
 * arch port (slice 5c) negates the returned value back into `errno`.
 */
#pragma once

#define EPERM           1   /* operation not permitted              */
#define ENOENT          2   /* no such file or directory            */
#define ESRCH           3   /* no such process                      */
#define EIO             5   /* I/O error                            */
#define ENOEXEC         8   /* exec format error (bad/oversized ELF) */
#define EBADF           9   /* bad file descriptor                  */
#define ECHILD         10   /* no child processes                   */
#define EAGAIN         11   /* try again (WNOHANG, no child ready)   */
#define ENOMEM         12   /* out of memory                        */
#define EACCES         13   /* permission denied                    */
#define EFAULT         14   /* bad address (user pointer)           */
#define EINVAL         22   /* invalid argument                     */
#define EEXIST         17   /* file/entry already exists            */
#define EMFILE         24   /* too many open files                  */
#define EPIPE          32   /* broken pipe: write with no reader (DDR-787) */
#define ESPIPE         29   /* illegal seek (non-seekable fd)       */
#define ERANGE         34   /* result too large for the buffer      */
#define ENAMETOOLONG   36   /* file name / string too long          */
#define ENODEV         19   /* no such device (e.g. no GPU framebuffer) */
#define ENOSYS         38   /* function not implemented             */
#define ENOSPC         28   /* no space left (bounded kernel table full) */
#define ETAMPER       133  /* DDR-812: record hash failed verification */
#define ENOKEY        126  /* DDR-834: key derivation failed (Linux value) */
