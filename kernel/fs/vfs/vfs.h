/* kernel/fs/vfs/vfs.h — minimal Virtual Filesystem Switch (Phase 4).
 *
 * Filesystem drivers register a vfs_fs_ops; vfs_mount probes registered drivers
 * against a block device. All access is capability-gated via the NCS: the caller
 * passes a capability bound to the FS resource carrying the needed right.
 */
#pragma once
#include <stdint.h>
#include "cap.h"

struct blk_device;                     /* defined in kernel/drivers/blk/blk.h */

#define FS_RES_ID 0x5346ull            /* 'FS' — resource id for FS capabilities */

struct vfs_file {
    uint64_t size;
    uint32_t cookie;                   /* FS-private (FAT32: start cluster) */
};

struct vfs_fs_ops {
    const char *name;
    int (*mount)(struct blk_device *bd);                                   /* 0 if recognized+mounted */
    int (*open)(const char *path, struct vfs_file *out);                   /* 0 ok */
    int (*read)(const struct vfs_file *f, uint64_t off, void *buf, uint32_t len); /* bytes, or -1 */
    int (*readdir)(const char *path, int index, char *name, uint32_t *size); /* 0 ok, -1 = end */
};

struct blk_device;

void vfs_register(const struct vfs_fs_ops *ops);
int  vfs_mount(unsigned blk_index);    /* try registered FS drivers on a disk */

int  vfs_open(cap_t cap, const char *path, struct vfs_file *out);
int  vfs_read(cap_t cap, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len);
int  vfs_readdir(cap_t cap, const char *path, int index, char *name, uint32_t *size);
const char *vfs_fs_name(void);         /* mounted FS name, or NULL */
