/* kernel/fs/vfs/vfs.h — Virtual Filesystem Switch (Phase 4).
 *
 * Filesystem drivers register a vfs_fs_ops. vfs_mount probes the registered
 * drivers against a block device and, on a match, records a mount in a small
 * mount table — each mount carries its own per-FS context (so FAT32/SFS/ext4
 * volumes can be mounted side-by-side). All access is capability-gated via the
 * NCS: the caller passes a capability bound to the FS resource carrying the
 * needed right (CAP_FS_READ / CAP_FS_WRITE).
 */
#pragma once
#include <stdint.h>
#include "cap.h"

struct blk_device;                     /* defined in kernel/drivers/blk/blk.h */

#define FS_RES_ID      0x5346ull       /* 'FS' — resource id for FS capabilities */
#define VFS_MAX_FS     4               /* registered filesystem drivers          */
#define VFS_MAX_MOUNTS 4               /* concurrently mounted volumes            */

struct vfs_file {
    uint64_t size;
    uint32_t cookie;                   /* FS-private (FAT32: first cluster, 0 = empty) */
    uint32_t dirent_clus;              /* FS-private (FAT32: cluster holding the dir entry) */
    uint16_t dirent_off;               /* FS-private (FAT32: byte offset of entry in cluster) */
    int      mnt;                      /* owning mount id (set by open/create)   */
};

/* A driver allocates a per-mount context in mount() and receives it back in
 * every other op. ctx-based ops keep all volume state out of file-scope globals
 * so multiple volumes (and multiple filesystems) can be mounted at once.
 * read returns bytes transferred (or -1); write returns bytes written (or -1);
 * the rest return 0 on success, -1 on failure. Unimplemented ops may be NULL. */
struct vfs_fs_ops {
    const char *name;
    int (*mount)  (struct blk_device *bd, void **ctx);                              /* 0 if recognized; *ctx = per-mount state */
    int (*open)   (void *ctx, const char *path, struct vfs_file *out);
    int (*create) (void *ctx, const char *path, struct vfs_file *out);             /* create empty regular file */
    int (*read)   (void *ctx, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len);
    int (*write)  (void *ctx, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len);
    int (*unlink) (void *ctx, const char *path);
    int (*readdir)(void *ctx, const char *path, int index, char *name, uint32_t *size);
};

void vfs_register(const struct vfs_fs_ops *ops);
int  vfs_mount(unsigned blk_index);    /* probe drivers on a disk -> mount id >= 0, or -1 */
const char *vfs_fs_name(int mnt);      /* mounted FS name for a mount id, or NULL */

/* Capability-gated operations. open/create/unlink/readdir take a mount id;
 * read/write take a vfs_file (which remembers its owning mount). */
int  vfs_open   (cap_t cap, int mnt, const char *path, struct vfs_file *out);
int  vfs_create (cap_t cap, int mnt, const char *path, struct vfs_file *out);
int  vfs_read   (cap_t cap, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len);
int  vfs_write  (cap_t cap, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len);
int  vfs_unlink (cap_t cap, int mnt, const char *path);
int  vfs_readdir(cap_t cap, int mnt, const char *path, int index, char *name, uint32_t *size);
