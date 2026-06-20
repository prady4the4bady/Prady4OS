/* kernel/include/stat.h — POSIX struct stat (Phase 5b, ADR-022).
 *
 * Field order + sizes mirror the Linux x86-64 `struct stat` so the slice-5c musl
 * port can use it unchanged (st_size is at offset 48). The kernel fills the
 * fields it can (size, a regular-file mode); the rest are zeroed for now.
 */
#pragma once
#include <stdint.h>

#define S_IFMT   0170000
#define S_IFREG  0100000   /* regular file */
#define S_IFDIR  0040000   /* directory    */

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;  uint64_t st_atime_nsec;
    uint64_t st_mtime;  uint64_t st_mtime_nsec;
    uint64_t st_ctime;  uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};
