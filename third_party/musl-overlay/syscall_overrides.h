/* third_party/musl-overlay/syscall_overrides.h — PROC-D, ADR-023 §D5/§D7.
 *
 * Appended to the generated build/musl/include/bits/syscall.h. The generator
 * (Makefile `musl` target) emits every musl x86_64 syscall number as
 * native+4096, so any number is >= MAX_SYSCALLS (64) and `syscall_dispatch`
 * returns -ENOSYS for it — a musl syscall we do NOT implement degrades safely
 * instead of mis-dispatching to an unrelated NSI handler (e.g. musl's native
 * ioctl=16 would otherwise hit SYS_WAIT4, rt_sigprocmask=14 -> SYS_EXECVE).
 *
 * Here we map the calls we DO implement back to their real NSI numbers. Keep in
 * sync with kernel/syscall/syscall.h. (arch_prctl is NOT here — the overlay
 * __set_thread_area.s issues SYS_SET_TLS directly; mmap needs no arg shim because
 * the kernel reads only 4 args, ignoring fd/off, which is correct for anon.) */
#undef  __NR_read
#undef  SYS_read
#define __NR_read   5
#define SYS_read    5

#undef  __NR_write
#undef  SYS_write
#define __NR_write  6
#define SYS_write   6

#undef  __NR_writev
#undef  SYS_writev
#define __NR_writev 28
#define SYS_writev  28

#undef  __NR_mmap
#undef  SYS_mmap
#define __NR_mmap   12
#define SYS_mmap    12

#undef  __NR_munmap
#undef  SYS_munmap
#define __NR_munmap 13
#define SYS_munmap  13

#undef  __NR_exit
#undef  SYS_exit
#define __NR_exit   4
#define SYS_exit    4

#undef  __NR_exit_group
#undef  SYS_exit_group
#define __NR_exit_group 4
#define SYS_exit_group  4
