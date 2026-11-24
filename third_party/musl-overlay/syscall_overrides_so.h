/* third_party/musl-overlay/syscall_overrides_so.h — DDR-1189 Amendment 5 (P3b-2).
 *
 * Read ONLY by tools/build_musl_so.sh (the shared libc.so / ld.so build), on top
 * of syscall_overrides.h. These are the further Linux calls musl's ld.so makes
 * that NSI already implements with Linux's argument layout and flag values.
 * The static libc.a (tools/build_musl.sh) does not read this file, so it and
 * every program linked with it are unchanged. Keep in sync with
 * kernel/syscall/syscall.h. Everything not listed in either file stays
 * native+4096 and returns -ENOSYS (pread, readlink, set_tid_address, ...). */
#define __NR_open      7    /* SYS_OPEN;  O_CLOEXEC accepted, no effect yet */
#define __NR_close     8    /* SYS_CLOSE */
#define __NR_fstat     9    /* SYS_FSTAT; Linux struct stat layout (stat.h) */
#define __NR_lseek     10   /* SYS_LSEEK */
#define __NR_mprotect  97   /* SYS_MPROTECT; W+X is -EACCES, never -ENOSYS */
