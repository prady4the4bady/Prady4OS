/* kernel/syscall/sys_io_uring.h — batched syscall ring (Phase 5b, PROC-E).
 *
 * A minimal io_uring: SYS_IO_URING_SETUP maps one shared ring page into the
 * calling process's address space and returns its user VA; SYS_IO_URING_ENTER
 * processes the first `to_submit` SQEs (OP_READ / OP_WRITE on FD_PIPE / FD_VFS /
 * console), writing one CQE per op. Baseline: no head/tail wrap, no kernel-side
 * polling thread — the agent runtime's high-throughput I/O path foundation.
 */
#pragma once

void sys_io_uring_register(void);
