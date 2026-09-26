# DDR-1037 — `SYS_POLL`: POSIX `poll()` over the existing readiness model

**Status:** DESIGN (implementation follows)
**Date:** 2026-09-02
**Queue:** Group D, `SYS_POLL` (the `SYS_FUTEX` half of that row stays open)

---

## §1 — What exists, checked rather than assumed

- **`SYS_POLL_RESULT` (NSI 32) is NOT this.** It is the AETHER action poll
  (`sys_aether.c:159`) and is unrelated to POSIX `poll()`. The name collision is
  close enough to mislead a reader, so it is called out here and in the header.
- **POSIX `poll()` does not exist**: no `sys_poll`, no `struct pollfd`.
- **epoll does** — NSI 19/20/21, `kernel/proc/epoll.c` — and it carries the only
  readiness predicate in the kernel: `fd_ready()` (`epoll.c:50`), `static`, which
  knows exactly one thing: `EPOLLIN` on an `FD_PIPE` read end with buffered data.

## §2 — The design decision this turns on

`poll()` and `epoll_wait()` must answer the *same question* about an fd. Two
readiness predicates that can disagree is precisely the "one source of truth"
failure this codebase already guards against elsewhere (the `PRADYOS_WM_GEOM`
block emits the hit-test's own expressions "so the emitted target cannot drift
from what the hit-test accepts").

**So `sys_poll` lives in `kernel/proc/epoll.c` and shares one predicate**, rather
than being a new file with a parallel copy. `fd_ready()` is generalised to
return a POSIX-shaped mask and both callers translate.

## §3 — The readiness table, including what it CANNOT answer

| fd kind | `POLLIN` | `POLLOUT` | notes |
|---|---|---|---|
| `FD_VFS` (regular file) | **always** | **always** | POSIX: a regular file never blocks. The current predicate returns 0 here, which would make `poll()` on a file **wrong**, not merely limited. |
| `FD_PIPE` read end | iff `pipe_has_data()` | — | today's behaviour, unchanged |
| `FD_PIPE` write end | — | iff `!pipe_full()` | `pipe_full()` already exists (DDR-787) |
| `FD_CONSOLE` | **never reported** | always | **A STATED GAP:** there is no console-input predicate in this kernel — no `console_has_data` of any name. So `poll()` on stdin can only ever report not-ready. Reporting it ready would be a lie; reporting `POLLNVAL` would be a different lie. Not-ready is the honest answer available, and it is recorded as a limitation rather than presented as support. |
| `FD_EPOLL` | 0 | 0 | not pollable; POSIX leaves this unspecified |
| invalid fd | — | — | `POLLNVAL`, per POSIX |
| `fd < 0` | — | — | entry ignored, `revents = 0`, per POSIX |

`POLLHUP` on a pipe read end whose writers have all closed (`pipe_writers() == 0`)
— `pipe_readers()`/`pipe_writers()` exist for exactly this (DDR-787).

## §4 — Timeout, and the one unbounded wait this creates

| `timeout` | behaviour |
|---|---|
| `0` | one pass, return immediately |
| `> 0` | bounded loop against a `g_ticks` deadline, `yield()` between passes |
| `-1` | loop until something is ready |

**The `-1` case is an unbounded wait and that is stated, not buried.** DDR-994
named unbounded waits as a defect class (`mnt_lock` is its example). The
difference here is that blocking forever is *what the caller asked for* — it is
POSIX's contract, not an accident of a spin loop — and it is **preemptible**,
because DDR-981 gave `yield()` an interrupt window. Refusing `-1` with `-EINVAL`
was considered and rejected: it would silently change the semantics of the one
argument value most `poll()` callers use.

`nfds` is bounded by `POLL_MAX_FDS` (32) — over that, `-EINVAL`. The array is
copied in, evaluated, and copied out; the caller's memory is never dereferenced
mid-loop.

## §5 — The gate: `smoke-poll`, and the mutants

Probe `user/polltest.c`. **Reports, then lets the gate judge** — no `fail()`
before a print.

| arm | exercises | sentinel |
|---|---|---|
| **A** | empty pipe, `timeout=0` → nothing ready | `PRADYOS_POLL_EMPTY n=0` |
| **B** | after a write, the read end is `POLLIN` | `PRADYOS_POLL_IN n=1 rev=1` |
| **C** | a bad fd yields `POLLNVAL`, not silence | `PRADYOS_POLL_NVAL rev=32` |
| **D** | a regular file is always ready — the POSIX case today's predicate gets wrong | `PRADYOS_POLL_FILE rev=5` |
| **E** | a positive timeout actually elapses | `PRADYOS_POLL_TMO n=0 waited=1` |

| mutant | must fail |
|---|---|
| **M1** drop the `POLLNVAL` branch | **C** |
| **M2** drop the timeout deadline (loop forever) | **E** — and the gate must time out cleanly rather than wedge; the loop yields, so the boot survives |
| **M3** report every fd ready | **A** |

Arm E is the one that needs care: it must assert *that time passed*, not just
that the result was 0 — otherwise a `poll()` that ignores its timeout entirely
and returns immediately passes it. The probe stamps `SYS_TIME` either side and
reports the delta, so the arm is a measurement rather than a restatement of the
return value.

## §6 — Scope NOT taken

- No `ppoll`, no signal mask.
- No `POLLPRI`, no out-of-band data — nothing in this kernel produces it.
- No socket fds: sockets are the ADR-027 proxy layer and are not `fd_entry`
  kinds, so they are outside this table entirely.
- **No console input**, per §3 — the missing predicate is a real gap and is
  listed in the pre-launch checklist rather than worked around.


## §7 — Measured

### §7.1 — Clean and mutants

Clean, kernel **`a411e1b1b765e15e`**:

```
PRADYOS_POLL_EMPTY n=0
PRADYOS_POLL_IN    n=1 rev=1
PRADYOS_POLL_NVAL  n=1 rev=32
PRADYOS_POLL_FILE  n=1 rev=5
PRADYOS_POLL_TMO   n=0 waited=2
PRADYOS_POLL_OK
```

| mutant | kernel | rc | missing sentinel — each lands on ITS OWN arm |
|---|---|---|---|
| **M1** drop the `POLLNVAL` branch | `0651ff30097d4617` | 2 | `PRADYOS_POLL_NVAL n=1 rev=32` (arm C) |
| **M2** drop the timeout deadline | `ac5415b49449cae3` | 2 | `PRADYOS_POLL_TMO n=0 waited=` (arm E) |
| **M3** report every fd ready | `f51b29901b3c8b10` | 2 | `PRADYOS_POLL_EMPTY n=0` (arm A) |

Three distinct missing sentinels means no mutant is carrying another. The tree
rebuilds to the clean hash after restoration, so nothing leaked between runs.

`waited=2` is the arm that matters most: the 2 s timeout genuinely elapsed. M2
removes only the deadline check and the gate then never sees the sentinel at all,
because the probe loops until the boot window expires.

### §7.2 — Arm E caught its OWN first draft, and that is the point

The first clean run reported **`PRADYOS_POLL_TMO n=0 waited=0`** and the gate
failed with *"the 2 s timeout did not elapse: poll() is ignoring it"*.

**`poll()` was correct. The probe was measuring nothing.** It stamped `SYS_TIME`
(NSI 72), which takes a `struct rtc_time *` out-pointer and returns `0`/`-EFAULT`
— so the probe differenced two identical return codes. `SYS_CLOCK` (NSI 57) is
the call that returns seconds **as a value**, and it is what DDR-1029 used.

This is a **near-miss on a vacuous arm, not a caught implementation bug**, and it
is worth separating the two: the code under test never had the defect the arm
appeared to report. What made it recoverable is that arm E asserts a *measured
quantity* rather than the return value. `n=0` was correct output; `waited=0` was
the measurement admitting it could not tell a `poll()` that waited from one that
did not. An arm that only checked `n=0` would have gone green over an unverified
timeout.

### §7.3 — Two harness defects that would have produced misleading results

**(a) `-Werror` rejected the mutants.** Deleting the deadline check leaves
`deadline` unused, so M2 and M3 did not compile. **A mutant that does not build
is not a mutant**, and `BUILD FAILED` scans past as though it had been tested.
Fixed with `(void)deadline;`, which keeps the mutation faithful — only the
behaviour is removed, not the variable.

**(b) The restore sat behind an early `return`.** On a build failure the harness
returned before restoring, so M2's mutation was still in the file when M3's patch
applied on top — which is why M3 reported the *same* `deadline` error for a
mutation unrelated to `deadline`. Had both compiled, M3 would silently have been
testing M2+M3 together and a pass would have proved nothing about either. The
restore is now unconditional.

Same class as DDR-1036 §8.2's concurrent-QEMU race: **a cleanup step reachable
only on the success path.** Both were caught because the results made no sense,
not because anything asserted them.

### §7.4 — What is NOT covered

- `timeout < 0` (block forever) has **no arm**. A gate that blocks until
  something becomes ready needs a second process to make it ready, and the probe
  is single-threaded. Recorded as uncovered, not claimed as tested.
- `POLLHUP` on a pipe whose writers have closed is implemented and unexercised —
  the probe never closes the write end.
- Console `POLLIN` is unimplementable here (§3) and therefore untested by
  construction.

### §7.5 — Regression, and the one that mattered

| gate | rc | why |
|---|---|---|
| `smoke-poll` | 0 | the new gate |
| **`smoke-sysepoll`** | **0** | **the one that mattered.** `fd_ready()` is no longer the predicate — it is a `mask & events` wrapper over `fd_ready_mask()`, so epoll's behaviour changed too, deliberately (`FD_VFS` now reports ready). A red here would have been a real finding about the shared predicate, not something to tune around. |
| `smoke-syspipe` | 0 | pipes are the other kind the predicate answers for |
| `smoke-shell` | 0 | §NON-NEGOTIABLE 4 |

`hygiene_check.sh`: all three PASSED. `ci-shard-check`: **170** gates / 10 shards
/ 7 excluded. `make image` rc=0, zero warnings at `-Werror`.