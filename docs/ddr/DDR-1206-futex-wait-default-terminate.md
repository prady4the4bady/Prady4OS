# DDR-1206: `FUTEX_WAIT` ends on a default-terminate signal

Status: **design, then implementation in the same PR.** It closes the known limit recorded in DDR-1181 A1.3 ("No gate covers this case ... the fix would be its own DDR"; #81 comments 6556392676 and 6556413334). It stacks on #130 (C1.5 P1), which adds the predicate this fix uses.
QEMU (TCG) only. Not verified on real hardware.

## 1. The defect

Read from the code at `915eb7c` (the #130 branch).

- The wait loop in `kernel/proc/futex.c` leaves on:
  - a `WAKE`;
  - the timeout;
  - `signal_interruptible(t)`, which needs a handler;
  - `signal_kill_pending(t)`, which is `SIGKILL` only.
- `sig_raise` calls `futex_kick`, so an unmasked `SIGTERM` or `SIGPIPE` with no handler does wake the waiter. Neither predicate matches, and the loop blocks again.
- `signal_deliver` ends a thread on those two signals (DDR-805), but only on the return to ring 3.
- So a waiter with no timeout is never terminated by `SIGTERM`. `kill <pid>` has no effect on a process blocked in `pthread_mutex_lock`, `sem_wait` or `pthread_join`, which all sit on this wait (`user/include/psync.h`, `pthr.h`).

#130 fixed the same defect for `nanosleep` (DDR-1188 A2-3) and added `signal_fatal_pending(t)`: an unmasked pending `SIGTERM` or `SIGPIPE` with no handler.

## 2. Decision

**D1.** The futex wait loop also leaves, with `-EINTR`, when `signal_fatal_pending(t)` is true.
- The check sits beside the other two, under the bucket lock, before each block. A signal raised after the check waits on that lock in `futex_kick` until the wait is published, as for the other two signals. No wake is lost.
- The thread never sees the `-EINTR`: `signal_deliver` ends it on the way out.
- A masked `SIGTERM` does not end the wait. `signal_fatal_pending` reads the deliverable set, as `signal_deliver` does.

## 3. Gate: arm FK in `smoke-thread`

- **The arm** (`user/threadtest.c`, after FJ).
  1. A forked child with no handler calls an untimed `FUTEX_WAIT`. It sets a ready flag first, and it records the wait's return value, which stays 99 while the wait has not returned.
  2. The parent waits for the flag, then for `SYS_GETPROCS` to report the child `THREAD_BLOCKED`, with the return value still 99.
  3. The parent sends `SIGTERM`.
  4. The child must be reaped within 2 s, with exit status -1 (ended by the kernel). The recorded return value must still be 99, because the child never ran again in ring 3.
- **Sentinel:** `PRADYOS_FUTEX_K term_waiter=1`.
- **Failure handling.** If the child is not reaped in 2 s, the parent ends it with `SIGKILL` and reaps it, so later arms still run and the red is on FK alone.
- **Vacuity.** On the unfixed tree the child blocks again and is never reaped, so `term_waiter=0`. Mutant F2-10 shows this in CI.
- **Mutant F2-10:** drop `signal_fatal_pending` from the futex loop.
- **Arms kept.** FG (`SIGKILL` ends a waiter) and FH (a handled signal gives `-EINTR`) are unchanged.

## 4. Audit: other waits with the same two-predicate check

`grep -rn signal_interruptible kernel` at `915eb7c`. These loops poll with `yield()`, which honours `SIGKILL` (DDR-1090) but not `SIGTERM`:

| Wait | Site | Ends on handlerless `SIGTERM`? |
|---|---|---|
| `nanosleep` | `kernel/syscall/sys_proc.c` | yes (#130, arm B3) |
| `FUTEX_WAIT` | `kernel/proc/futex.c` | yes (this DDR, arm FK) |
| pipe write, ring full | `kernel/syscall/sys_io.c:68` | **no** |
| pipe read, ring empty | `kernel/syscall/sys_io.c:387` | **no** |
| console read | `kernel/syscall/sys_io.c:421` | **no** |
| `epoll_wait`, no timeout | `kernel/proc/epoll.c:257` | **no** |

- The four "no" rows are **not changed here**. Each needs its own arm and mutant.
- They are read from the code, not reproduced.
- They are recorded as the follow-up to this DDR.
- `wait4` and the other blocking paths were not audited. That is stated so the table is not read as complete.

## 5. Risks

- A program that relied on a handlerless `SIGTERM` being ignored while it waits now terminates. That is the POSIX default action, and it is what the same program already gets when it is not blocked.
- Size: one call in the loop, and one probe arm.
