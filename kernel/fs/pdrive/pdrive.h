/* kernel/fs/pdrive/pdrive.h — PRADYOS Drive (Group 7 item 40, DDR-890). */
#pragma once

/* Register the RAM-backed workspace filesystem with the VFS. Mounted via
 * vfs_mount_virtual("pdrive") — it has no block device and deliberately
 * refuses to claim one. */
void pdrive_register(void);
