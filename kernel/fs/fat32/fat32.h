/* kernel/fs/fat32/fat32.h — FAT32 read-only driver (Phase 4 starter). */
#pragma once

/* Register the FAT32 driver with the VFS (probes a disk on vfs_mount). */
void fat32_register(void);
