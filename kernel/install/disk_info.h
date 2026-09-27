/* kernel/install/disk_info.h -- SYS_DISK_LIST (NSI 104) wire format
 * (DDR-1143 sec.10.6/10.8). Ring-3 callers hand-copy this layout; it is
 * pinned by _Static_assert in sys_disk.c, so a change here fails the build
 * rather than overflowing a caller's buffer. */
#pragma once
#include <stdint.h>
#include "string.h"

#define DISK_F_PHYS       0x01u  /* a real controller (virtio-blk/nvme/ahci): installable */
#define DISK_F_RAMDISK    0x02u
#define DISK_F_PART       0x04u  /* a partition sub-device */
#define DISK_F_BLANK      0x08u  /* sector 0 is all zero */
#define DISK_F_INSTALLED  0x10u  /* MBR carries the PRADYOS install signature */
#define DISK_F_READERR    0x20u  /* sector 0 could not be read: no content flag is claimed */

/* MBR disk-signature bytes at offset 440 on an installed disk. */
#define DISK_INSTALL_SIG_OFF 440u
#define DISK_INSTALL_SIG     "PRDI"

struct disk_info {
    char     name[16];   /* driver name, NUL-terminated ("virtio-blk", ...) */
    uint64_t sectors;    /* 512-byte sectors */
    uint32_t flags;      /* DISK_F_* */
    uint32_t index;      /* registry index: the idx SYS_INSTALL takes */
};

/* The PHYS allowlist, shared by SYS_DISK_LIST (which reports it) and
 * SYS_INSTALL (which requires it), so the two cannot drift: a device the
 * lister calls non-physical must never be one the installer will wipe. */
static inline int disk_name_phys(const char *n) {
    return n && (strcmp(n, "virtio-blk") == 0 || strcmp(n, "nvme0") == 0 ||
                 strcmp(n, "ahci") == 0);
}

/* SYS_INSTALL (NSI 105) layout, DDR-1143 sec.3 / sec.10.6. */
#define INST_ESP_LBA     4096u
#define INST_ESP_SECT    131072u                       /* 64 MiB FAT16 ESP */
#define INST_P2_LBA      (INST_ESP_LBA + INST_ESP_SECT) /* 135168 */
#define INST_P2_SFS_OFF  8u                            /* header sectors */
#define INST_MIN_SECT    (INST_P2_LBA + 32768u)         /* 80 MiB */
#define INST_VOL_MAGIC   "PRDYVOL1"
#define INST_VOL_PLAINTEXT 0x1u   /* DDR-1144 replaces this with a crypt header */
#define INST_MARK_PATH   "/INSTALLED.MARK"
