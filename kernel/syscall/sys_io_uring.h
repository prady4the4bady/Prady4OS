/* kernel/syscall/sys_io_uring.h — batched syscall ring (Phase 5b, PROC-E).
 *
 * A minimal io_uring: SYS_IO_URING_SETUP maps one shared ring page into the
 * calling process's address space and returns its user VA; SYS_IO_URING_ENTER
 * processes the first `to_submit` SQEs (OP_READ / OP_WRITE on FD_PIPE / FD_VFS /
 * console), writing one CQE per op. The ring VA must be PAGE-ALIGNED (DDR-1108);
 * an unaligned one is refused with -EINVAL.
 *
 * BASELINE, and this wording is corrected by DDR-1108 §5 because the old "no
 * head/tail wrap" understated it twice over. `sq_head`, `sq_tail` and `cq_head`
 * have ZERO kernel writers and ZERO kernel readers: SETUP zeroes the page and
 * writes only `entries`; ENTER writes only `cq_tail`, and does so by ASSIGNMENT.
 * ENTER always runs sqes[0 .. to_submit) and always writes cqes[0 .. done) --
 * indexed from zero on EVERY call, not from a head. So this is not a ring whose
 * indices fail to wrap; it is a fixed array whose index fields are inert. Two
 * consequences, neither of them a wrap: a caller following the real io_uring
 * protocol (publish at sq_tail, bump it, enter) gets the WRONG SQE executed from
 * its second call onward, silently -- index 0 and sq_tail coincide only on the
 * first call, which is exactly why the shipped probe works; and a second ENTER
 * overwrites cqes[0..done), destroying completions not yet consumed.
 *
 * NOT FIXED, on DDR-1069's test rather than difficulty: a real index discipline
 * is a ring rewrite plus an ABI contract, and nothing shipping needs it -- the
 * one ring-3 consumer issues a single ENTER and never reads an index. Also no
 * kernel-side polling thread. The agent runtime's high-throughput I/O path
 * foundation.
 */
#pragma once

void sys_io_uring_register(void);
