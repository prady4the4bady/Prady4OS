/* kernel/ipc/shm.h — SysV shared-memory segment objects (DDR-1178 Amendment 2).
 * The object table and its lifetime rules; the syscalls that map segments live
 * in kernel/syscall/sys_mmap.c beside the other address-space calls. */
#pragma once
#include <stdint.h>

#define SHM_MAX        32          /* segment objects, system-wide          */
#define SHM_MAX_PAGES  256         /* 1 MiB per segment                     */

struct mm;

/* Create an object of `npages` zeroed frames. Returns its index, -ENOSPC when
 * the table is full, -ENOMEM when frames run out. holders = nattch = 0: the
 * caller mints the capability (which holds it) straight after. */
int  shm_obj_create(uint32_t npages);

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

/* Capability-table holds (cap.c): a table slot naming the object. */
void shm_hold(int obj);
void shm_unhold(int obj);

/* fork: the child's copied VMA table attaches every shm VMA once more. */
void shm_fork_vmas(struct mm *child);
/* exec / last mm_put: detach every shm VMA of `mm` and clear its marks. */
void shm_release_vmas(struct mm *mm);
