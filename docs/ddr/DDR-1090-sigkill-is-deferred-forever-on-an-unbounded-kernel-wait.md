# DDR-1090 — SIGKILL is deferred forever on an unbounded kernel wait

**Status:** artefact produced, fixed, gated (M1 = the pre-fix tree, bit-identical)
**Date:** 2026-09-07

---

## 1. The finding, measured

`signal_deliver()` is called from exactly two places, and **both are guarded by
`(r->cs & 3) == 3`**:

* `kernel/idt.c:532` — the BSP timer-IRQ return, *"to the ring-3 thread we're
  returning to"*
* `kernel/idt.c:672` — the AP timer-IRQ return (cap-4)

So a signal is acted on **only when the interrupted frame is a ring-3 frame**. A
thread inside a syscall is in ring 0; a timer IRQ that lands there calls
`signal_deliver`, which returns at its first line.

That is correct and cheap for a syscall that finishes. **It is not correct for a
syscall that never finishes**, and this kernel has several that need not:

| site | wait | bound |
|---|---|---|
| `sys_io.c:60` | `pipe_write` while the ring is full and a reader exists | **none** — its own comment: *"A reader that exists but never reads leaves this spinning forever"* |
| `sys_io.c:338` | `pipe_read` while empty and a writer exists | **none** — *"A writer that exists but never writes leaves this spinning forever"* |
| `sys_io.c:366` | blocking console read | **none** — `for (;;)` |
| `epoll.c:254` | `poll`/`epoll_wait`, `timeout < 0` | **none, by the caller's request** |
| `vfs.c:46` | `mnt_lock` acquire | **none** (DDR-994 instrumented it; it did not bound it) |

A thread in any of those is in ring 0 at **every** timer IRQ, forever. So
`sig_pending |= 1<<SIGKILL` is recorded and **never acted on**.

## 2. Three source comments state something stronger than what is delivered

This is the DDR-1046/1059/1070/1080/1089 class — *a control that cannot act in
the case it exists for* — and here the case is the one the control was built for.

1. **`kernel/proc/signal.h:14`** — *"SIGKILL (9, **unblockable** terminate)"*.
   It is blockable, by any unbounded syscall, indefinitely.
2. **`kernel/syscall/sys_aether.c:281`** — `sys_kill_agent` sets the bit with
   *"terminated on its next IRQ return"*. Measured, it is terminated on its next
   **ring-3** IRQ return. **This is the sovereign's kill switch on a runaway
   agent** (`is_sovereign || is_agent`), so the enforcement mechanism against a
   misbehaving agent is exactly what an agent wedged in an unbounded wait
   escapes. That is the expensive half of this finding.
3. **`kernel/proc/epoll.c:250-253`** — *"blocking forever is the caller's
   explicit request, and `yield()` carries an interrupt window since DDR-981, so
   **the CPU is not wedged**"*. The CPU is indeed not wedged and that sentence is
   true as far as it goes; what it does not say is that **the thread is now
   unkillable**, which is a different property and the one that matters here.

SIGKILL exists precisely to stop a process that will not stop on its own. The
one situation where that is needed is the one situation where it cannot be
delivered.

## 3. `SA_RESTART` has no subject, for the same structural reason

Group D's `SYS_SIGACTION` row asks for `SA_RESTART`, `SA_SIGINFO`,
`sigprocmask`, `sigaltstack` and `SIGCHLD`. Measured, and this is the Group G
9.3 shape (a row with no subject):

* **`SA_RESTART` — no subject.** `grep -rn EINTR kernel/ user/` returns
  **nothing**. A signal cannot interrupt a syscall here, so no syscall can
  return `-EINTR`, so there is nothing to restart. **And the coupling is worth
  writing down: fixing §1 by unwinding blocked syscalls is exactly what would
  create `EINTR` and give `SA_RESTART` a subject.** The two rows are one item.
* **`sigaltstack` — no subject.** Its purpose is to handle the stack-overflow
  signal on a separate stack, and `grep -rn SIGSEGV` returns **nothing**: a
  ring-3 fault goes straight to `sched_exit(-1)`.
* **`SA_SIGINFO` — buildable and would be mostly zeros.** `sys_kill` carries
  `(pid, sig)` only; no sender pid and no fault address are recorded anywhere,
  so a `siginfo_t` would be a struct of zeros with a real-looking name — the
  DDR-1059 shape, a control that reads stronger than it is.
* **`sigprocmask` — genuinely buildable and genuinely absent.** There is no
  mask: `struct tcb` carries `sig_pending`, `sig_handlers[32]`, `sig_saved` and
  a one-bit `sig_active`. Note `sig_active` is a de-facto *all signals blocked
  while in a handler*, which is **stronger** than the POSIX default.
* **`SIGCHLD` — buildable with no ABI change** (the handler table is already 32
  wide, so any POSIX number works today; what is missing is a raiser in
  `sched_exit`). **Nothing shipping needs it**, measured: PRISM's `jobs_reap()`
  polls `wait4` at every prompt and DDR-891's init service manager polls
  `wait4(-1, …, WNOHANG)`. By DDR-1069's own test — *a feature nothing shipping
  needs, exercised only by its own gate, is the wrong item* — it is not built
  here.

**Nothing in §3 is fixed by this DDR.** It is recorded so the row stops reading
as five equivalent unbuilt features when two of the five have no subject at all.

## 4. The artefact (§NON-NEGOTIABLE 3)

§1 is a reading of the source. A reading is not an artefact, so one is produced
before anything is changed.

`user/killblocktest.c`:

1. `pipe(fds)`; `fork()`.
2. **Child** closes the write end, prints `PRADYOS_KILLBLOCK_CHILD blocking`,
   then `read(read_end, …)`. The **parent still holds the write end**, so
   `pipe_writers() > 0` and the `sys_io.c:338` loop spins forever. Deterministic,
   self-contained, needs no console injector.
3. **Parent** spins in ring 3 until the child's marker can have been printed,
   `SYS_KILL(child, SIGKILL)`, spins again, then
   `wait4(child, &st, WNOHANG)`.

The discriminating value is `wait4`'s return, **from the kernel** — not a flag
the probe reports about itself (the DDR-1066/1084 discipline).

**MEASURED, two-sided, same probe and same child pid:**

| kernel | capture | reading |
|---|---|---|
| pre-fix `f314ed83a59c042d` | `PRADYOS_KILLBLOCK child=38 reaped=-11` | the child is **still alive** 3 wall seconds and ~300 timer IRQs after SIGKILL |
| fixed `fb09a2cd12f92b90` | `PRADYOS_KILLBLOCK child=38 reaped=38` | reaped |

**M1 is the pre-fix tree itself**, not a synthetic defect (the DDR-1066/1067
form): disabling the new guard rebuilds to **`f314ed83a59c042d` bit-for-bit**,
and against the corrected arm it fails with its output reproducing the artefact
verbatim — *"FAIL — forbidden pattern 'reaped=-' appeared"* over
`PRADYOS_KILLBLOCK_CHILD blocking` / `child=38 reaped=-11`.

**Vacuity checked before writing it** (tenth time in design text): a not-reaped
answer is also what a correct kernel gives if **the child never reached the
block** — so the child prints its own marker *first*, and the gate requires both.
Without that, "the kill was deferred" and "the fork lost the race" are the same
observation.

### 4.1 — and the forbidden arm was STILL vacuous, which only RUNNING it caught

The first form forbade **`reaped=0`**. **This kernel does not answer `0`:**
`sys_wait.c:62` returns **`-EAGAIN`** for *"children exist, none exited yet"*.
So the pre-fix capture read `reaped=-11`, the forbidden pattern did not match,
and **the gate went GREEN on the very tree whose defect it exists to catch** —
`rc=0`, `[killblock] PASS — PRADYOS_KILLBLOCK child=38 reaped=-11`.

Checking a new arm against the *product* is not enough (DDR-1085 §5.1 made the
mirror-image mistake, checking it against the product but not against the gate's
other required patterns). Here the arm was checked against neither: it was
checked against **POSIX**, which says `waitpid` with `WNOHANG` returns 0, and
this kernel deliberately does not. The corrected arm forbids **`reaped=-`** —
every negative — so it is robust to *which* errno the kernel picks rather than
to one guess about it.

**And `-EAGAIN` is a stronger artefact than `0` would have been**: `-ECHILD`
means no such child, so `-EAGAIN` says positively that **the child still exists
and has not exited**.

### 4.2 — the first probe could not reach its own kill, and that is recorded too

The first draft timed the parent's two waits with a fixed `pause` count
(200,000,000). Under TCG that outran the gate's whole 120 s window: the capture
showed `PRADYOS_KILLBLOCK_CHILD blocking` and **nothing else**, because the
parent never reached the `SYS_KILL`. An instruction count is a guess about
emulation speed. Replaced with `SYS_CLOCK` wall seconds — DDR-1068/DDR-1029's
own precedent, bounded two ways because a wall clock wraps at midnight.

Both arms print unconditionally, on **one line via one `write(2)`** (DDR-1020
report-and-judge + DDR-1056's uline rule).

## 5. The fix, and why the obvious hazard does not apply here

`yield()` is the choke point **all six sites share** — which is DDR-981's own
finding, reached for the same reason (*"fixing `sys_yield` alone would not have
fixed the observed livelock, which was in `mnt_lock`"*).

The obvious objection is that abandoning a caller's stack frames from inside
`yield()` leaks whatever it held. **Measured at every ring-3-reachable site, and
it does not apply:**

* `sys_io.c:60/338/366` — stack `kbuf[256]`, no lock, no allocation. (The
  `pmm_alloc_page` in `fd_write_user` is in the **FD_VFS** branch, which calls
  `vfs_write` directly and never yields — checked, not assumed.)
* `epoll.c:254` — stack arrays only.
* `syscall.c:204` — `sys_yield`, nothing held.
* `vfs.c:46` — the yield is in the **acquire** loop, so `mnt_lock` is *not*
  held; a thread exiting there was never the owner.

`sched_exit()` from mid-kernel is an established pattern in this tree, not a new
one: `idt.c:703` calls it on a ring-3 fault from inside the ISR, and
`signal_deliver` itself calls it from the IRQ return path.

Guarded on `is_user` so kernel threads (the `main.c` self-tests, which yield
heavily) are untouched.

**Three source comments are corrected in the same commit** (§2), because a
comment that over-states a guarantee is what let this survive: `signal.h` no
longer calls SIGKILL *"unblockable"* and now records what the delivery guard
does and does not provide; `sys_aether.c` says *"on its next **ring-3** IRQ
return, or at its next `yield()`"*; `epoll.c` records that *"the CPU is not
wedged"* and *"the thread is killable"* are different properties. Those edits are
**comment-only and verified so**: the kernel rebuilt to `fb09a2cd12f92b90`,
bit-identical to the build before them.

## 6. NOT CLAIMED

* **No general `EINTR` semantics.** SIGKILL terminates; every other signal stays
  deferred to the next ring-3 return exactly as today. A catchable signal
  interrupting a blocked syscall would need each loop to unwind with a return
  code, which is the larger change §3 describes and is **not** made here.
* **The unbounded waits are not bounded.** DDR-994 instrumented them and
  deliberately did not bound them; that stands. This changes only whether a
  thread sitting in one can be killed.
* **`sigprocmask`, `SA_SIGINFO` and `SIGCHLD` are not built**, and §3 records why
  for each rather than leaving the row undifferentiated.
* **Nothing is claimed about OPEN-1 or OPEN-2.** `mnt_lock` is on OPEN-1 route
  1's path and this makes a thread stuck there killable; it does **not** say why
  anything gets stuck, and no open issue moves.
