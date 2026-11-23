/* kernel/ipc/shm.h — SysV shared-memory segment objects (DDR-1178 Amendment 2).
 * The object table and its lifetime rules; the syscalls that map segments live
 * in kernel/syscall/sys_mmap.c beside the other address-space calls. */
#pragma once
#include <stdint.h>

#define SHM_MAX        32          /* segment objects, system-wide          */
#define SHM_MAX_PAGES  256         /* 1 MiB per segment                     */

struct mm;
struct vm_area;
struct vfs_file;

/* Create an object of `npages` zeroed frames. Returns its index, -ENOSPC when
 * the table is full, -ENOMEM when frames run out. holders = nattch = 0: the
 * caller mints the capability (which holds it) straight after. */
int  shm_obj_create(uint32_t npages);

/* DDR-1178 Amendment 3 (P3): reserve an EMPTY object (npages 0) held once by
 * the caller's new fd, and size it exactly once. shm_obj_size returns -EINVAL
 * when the object is already sized or being sized, -ENOMEM when frames run out
 * (the object stays empty). */
int  shm_obj_reserve(void);
int  shm_obj_size(int obj, uint32_t npages);

/* Frame `i` of object `obj` (a physical address), or 0. */
uint64_t shm_obj_frame(int obj, uint32_t i);
uint32_t shm_obj_npages(int obj);

/* Attach bookkeeping. shm_attach_begin fails with -EIDRM on a marked segment
 * and -EINVAL on a free slot; every successful begin is paired with exactly one
 * shm_detach (shmdt, munmap, exec, or the address space's last reference). */
int  shm_attach_begin(int obj);
void shm_detach(int obj);

/* IPC_RMID: no new attaches; destroyed at the last detach. */
int  shm_mark(int obj);

/* Holds: a capability-table slot naming the object (cap.c, P2), or an FD_SHM
 * fd entry (fd.c / pipe.c dup2, P3). */
void shm_hold(int obj);
void shm_unhold(int obj);

/* fork: the child's copied VMA table attaches every shm VMA once more. */
void shm_fork_vmas(struct mm *child);
/* exec / last mm_put: detach every shm VMA of `mm` and clear its marks. */
void shm_release_vmas(struct mm *mm);

/* ---- DDR-1178 Amendment 4 (P4): shared file mappings ---------------------
 * shm_file_attach: attach (nattch++) the FILE object for (f->mnt, ident),
 * creating it by reading the whole file with `rcap` if none is live. Returns
 * the object index, -EINVAL (empty file), -EFBIG (> SHM_MAX_PAGES), -ENOSPC,
 * -ENOMEM or -EIO. Paired with one shm_detach, as every attach. */
int  shm_file_attach(const struct vfs_file *f, uint64_t ident, uint64_t rcap);
/* 1 while a FILE object for (mnt, ident) exists: the file is busy (A4.2). */
int  shm_file_busy(int mnt, uint64_t ident);
/* 1 while any FILE object exists on `mnt` (a cheap pre-check). */
int  shm_file_any(int mnt);
int  shm_file_grow_busy(int mnt, uint64_t ident, uint64_t end);   /* A4.8 */
int  shm_vfs_grow_refused(const struct vfs_file *f, uint64_t off, uint64_t len);
int  shm_obj_is_file(int obj);

/* A4.4: write back the pages of area `v` (address space `cr3`) inside
 * [from, to) whose PTE is dirty. SHM_WB_MSYNC prepays the bytes, clears D and
 * shoots down the TLB first, and returns -EAGAIN / -EIO; SHM_WB_RELEASE
 * (munmap, exec, last mm_put) only tests D, is prepaid at mmap, and reports a
 * failure on the console. 0 for a read-only or non-FILE area. */
#define SHM_WB_MSYNC   0
#define SHM_WB_RELEASE 1
int  shm_vma_writeback(uint64_t cr3, const struct vm_area *v, uint64_t from,
                       uint64_t to, int mode);
/* exec (before the commit point) and the last mm_put: write back every
 * writable FILE area of `mm`. Detaching stays shm_release_vmas's job. */
void shm_writeback_vmas(struct mm *mm);
