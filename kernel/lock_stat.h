/* kernel/lock_stat.h — DDR-1047: spinlock contention accounting.
 *
 * WHAT IS MEASURED, AND WHY IT IS THIS AND NOT HOLD TIME.
 *
 * The Group A row asks for "lock_stat hold-time + contention counts". Those two
 * have very different costs, and the difference decides the design:
 *
 *   - CONTENTION is observable entirely in the SLOW path. A lock that is taken
 *     uncontended never enters it, so an uncontended acquire pays NOTHING.
 *   - HOLD TIME needs a timestamp at every acquire AND every release, i.e. an
 *     rdtsc pair on the kernel's hottest primitive, paid by every acquisition
 *     whether contended or not.
 *
 * That cost is not acceptable here, and the reason is specific rather than
 * general: OPEN-2 is a timing-sensitive AP freeze, and DDR-1010 recorded this
 * exact hazard about its own probe — "it adds work to the very syscall path
 * where the race lives, so it may perturb what it measures". An always-on
 * rdtsc pair in spin_lock could move that bug rather than measure it.
 *
 * Making it opt-in is NOT the escape: DDR-1010's other rule is that an opt-in
 * instrument is guaranteed OFF in CI, which is the only place OPEN-2 has ever
 * appeared (DDR-1043 found precisely that, for the QMP dump).
 *
 * So: WAIT time, not hold time. Both timestamps live in the slow path, so the
 * fast path is untouched — and for the open question ("which lock is a wedged
 * AP stuck on?") wait time is the direct measurement anyway. A frozen CPU is
 * one that is WAITING.
 *
 * WHY A SIDE TABLE AND NOT FIELDS IN spinlock_t. Growing spinlock_t would shift
 * every struct that embeds one — and this kernel has assembly-visible fixed
 * offsets (percpu.h documents @0/@8/@16 and @56..@120, static-asserted in
 * percpu.c because syscall_entry.asm reads them). A side table keyed by lock
 * address changes no layout at all, so that whole class of risk does not arise.
 *
 * DDR-1060 CORRECTS THE ORDERING THIS FILE ORIGINALLY SHIPPED. The paragraph
 * above is right that "a frozen CPU is one that is WAITING" -- but the first
 * implementation recorded everything AFTER the lock was acquired, so it counted
 * only COMPLETED waits and was blind to the wedged AP, i.e. to the exact case
 * named here as its purpose. The slot is now claimed BEFORE the spin and a live
 * `waiters` count is incremented there, so a frozen CPU leaves a permanent +1
 * on the lock it is stuck on. The fast path is still untouched.
 *
 * The table is lock-free by necessity: taking a lock to record lock statistics
 * would recurse. Insert is a CAS on an empty key; everything else is a relaxed
 * add. Bounded at LOCK_STAT_SLOTS with an overflow counter, so a kernel with
 * more contended locks than slots reports that fact instead of lying.
 */
#pragma once
#include <stdint.h>

#define LOCK_STAT_SLOTS 32

/* DDR-1060: a slot's unit. Spin waits are cycles this CPU burned; yield waits
 * are wall time during which it ran other threads. The two are NOT
 * commensurable -- DDR-1047 kept mnt_lock out of this table rather than put
 * both into one waitavg column, and the dump keeps that boundary by giving each
 * kind its own line shape instead of a shared column. */
#define LS_KIND_SPIN  0
#define LS_KIND_YIELD 1

/* The contended path of spin_lock. Out of line on purpose: the fast path stays
 * one test-and-set plus a branch, which is no more than the original spin loop
 * emitted at each call site. */
void spin_lock_contended(void *lock);

/* DDR-1060 §5: account a wait on something that is NOT a spinlock_t -- a
 * sleep-mutex spun on with yield(), of which mnt_lock (kernel/fs/vfs/vfs.c) is
 * the one that matters: DDR-994 and PRE_LAUNCH_CHECKLIST §4.11 name it as the
 * unbounded wait on OPEN-1 route 1's path, and it was the lock this table could
 * not see. Contributes to the live waiter count and to a completed-wait count,
 * and to NO cycle column. Call end() only after the wait succeeds. */
void lock_wait_begin(void *lock);
void lock_wait_end(void *lock);

/* Print the contended locks. SLOT ORDER, not sorted: this runs on the freeze
 * path, and a sort is work added at the worst possible moment for the sake of
 * presentation. Output is bounded by LOCK_STAT_SLOTS, and slots with zero hits
 * are skipped, so a healthy subsystem costs no lines. */
void lock_stat_dump(void);
