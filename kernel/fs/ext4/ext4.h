/* kernel/fs/ext4/ext4.h — ext4 read-only compatibility driver (Phase 4j).
 *
 * Enough ext4 to mount a volume, walk the root (and nested) directories, and
 * read regular files via the extent tree. Read-only: create/write/unlink are
 * not provided. Modern mkfs.ext4 (extents + optional 64-bit) is supported;
 * legacy block-mapped inodes and multi-level extent trees are later work.
 */
#pragma once

void ext4_register(void);   /* register the ext4 driver with the VFS */
