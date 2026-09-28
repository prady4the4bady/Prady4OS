# DDR-1056 — the same splice one ring out: ring-3 measured lines, and musl's
# two-iovec fflush

**Status:** IMPLEMENTED + detector + a DETERMINISTIC measurement
**Date:** 2026-09-03
**Branch:** `dev/phase1-seyp3n`
**Depends on:** DDR-1055, which fixed the ring-0 half and named this in its §9.

---

## 1. Why this is a second DDR and not a wider DDR-1055

DDR-1055 asked one question of all 268 `EXTRA_SENTINEL` patterns — *does any
single string literal contain it?* — and fixed the ring-0 composites. That sweep
had a hole, and the hole is where a second CI failure lives: **a gate asserts in
two places**, and the second is the Makefile recipe's post-check `grep`, which
usually matches the *whole* measured line rather than a prefix.

```make
ln=$(grep -ao "PRADYOS_ACTIONDEL_OK id=[0-9]* st=-*[0-9]* ctrl=[0-9]* keep=[0-9]*" build/actiondel.log | head -1)
test -n "$ln" || { echo "[actiondel] FAIL — no measured line in the capture"; exit 1; }
```

`PRADYOS_ACTIONDEL_OK id=` does sit in one literal, so DDR-1055's sweep called it
safe. The post-check does not care: `user/actiondeltest.c:172` builds that line
from nine `wr()`/`wrdec()` calls — and **that is eleven `write(2)`s, not nine**,
because `wrdec` emits **one digit per write**:

```c
while (i--) { char c[2]; c[0] = b[i]; c[1] = 0; wr(c); }
```

So `PRADYOS_ACTIONDEL_OK id=258 st=1 ctrl=1 keep=1` is five literal writes plus
one per digit — `id=258` alone costs three. Eleven `g_console_lock`
acquisitions, **ten gaps**, and the wider the number the more gaps. Every probe
in the tree rolls its own `wr()` as one `SYS_WRITE` and most have this same
digit-at-a-time `wrdec`.

## 2. Three distinct splice paths, all measured from the tree

**(a) Probe lines built from many `wr()` calls.** The actiondel case above, plus
`actionhypo`, `actionquery`, `actionread`, `actionspawn`, `fat32mc`, and the five
`polltest` / five `exptest` / three `ipctest` measured lines.

**(b) musl's `fflush` emits TWO console writes, not one.**
`third_party/musl/src/stdio/__stdio_write.c` always passes **two** iovecs — the
already-buffered bytes and the new bytes — and `kernel/syscall/sys_io.c`'s
`sys_writev` calls `fd_write_user` **per iovec**, i.e. a separate `kwrite` and a
separate console-lock acquisition each. So any musl-linked program (the
compositor, PRISM, `term`, `cmusl`, `agent_base`, `init`) can have a line split
in the middle whenever the stdio buffer was non-empty at flush time.

Buffering was checked in this tree rather than recalled: `__stdout_write` sets
`f->lbf = -1` (fully buffered, `BUFSIZ` 1024) when `ioctl(TIOCGWINSZ)` fails, and
this kernel registers **no** `SYS_IOCTL` at all, so stdout here is always fully
buffered. The compositor has 71 `printf`s and 52 `fflush`es, so bytes do
accumulate between flushes and the two-iovec case is the normal one, not a
corner.

**(c) `fd_write_user` chunks the console at 256 bytes.** A single `write()`
longer than that is already several acquisitions.

## 3. Fix

- **(a)** `user/include/uline.h` — build the measured line into a stack buffer,
  hand `ul_end()` to the probe's own `wr()`: one call, one write. Signed decimal,
  because gates match `st=-1` and `rc=-40`. Overflow appends `[uline] TRUNC`,
  added to `GLOBAL_FORBIDDEN` (again inserted before the final list line so
  §NON-NEGOTIABLE 6's terminator does not move).
- **(b)** `sys_writev` on an `FD_CONSOLE` fd gathers the iovecs into one bounce
  buffer and issues a single `kwrite` when the total fits; otherwise it falls
  back to today's per-iovec loop. This removes the split for every musl program
  without touching the buffering discipline or adding a lock.
- **(c)** NOT fixed. Recorded: a console write longer than the bounce buffer is
  still several acquisitions. No gate asserts a line that long, and fixing it
  means holding a lock across a UART busy-wait proportional to the payload.

## 4. Gate diagnosability defect fixed in passing

`smoke-actiondel`'s failure path prints only `no measured line in the capture`
and exits. It does **not** dump what it *did* find, which is why the CI log for
`c8c93ed` cannot settle whether that failure was this defect — a splice and a
probe that never ran look identical from outside. Compare `smoke-nethammer`,
which dumps its `NETHAMMER` lines on failure and is why DDR-1055 was diagnosable
at all. The failure path now dumps the matching-prefix lines.

## 5. What is NOT claimed

- The `smoke-actiondel` failure is **not attributed** to this defect. The
  mechanism matches and the timing matches (same push, same docs-only commit,
  `kernel.bin: OK`), but the capture's own `PRADYOS_ACTIONDEL_OK` line was never
  read back, and a matching mechanism is not an attribution — DDR-1019's rule
  about `[apfreeze]`'s three producers.
- The `smoke-surfclose` failure on `1efbb49` is **not** claimed as this defect
  either. It is a compositor line, so path (b) applies in principle, but the
  failing assertion was not identified from the job log.


---

## 6. Proof — deterministic, not statistical

Unlike DDR-1055 there is **no locally reproducing failure** here, so a rate
campaign would measure nothing: I cannot show a splice I cannot provoke. What
*can* be measured exactly is the quantity the fix changes — **how many
`write(2)` calls the measured line costs** — and the kernel already counts it.
`sys_exit` prints `writes=`, DDR-948's per-thread write-attempt counter.

| | probe pid | `writes=` |
|---|---|---|
| before (`5f0a2f60d56fbd9b`) | 36 | **13** |
| after (`7ff8150dd2697358`) | 37 | **3** |

A drop of exactly **10**, which is 11 writes for the measured line collapsing to
1, with the probe's two other writes unchanged. That is the whole claim of part
(a), measured rather than argued, and it does not depend on winning a race.

`smoke-actiondel` rc=0 on both, with the same measured line
(`id=258 st=1 ctrl=1 keep=1`), so the fix did not change what the probe reports —
only how many acquisitions it takes to report it.

**Regression: 16/16 rc=0**, `kernel_after == kernel` (`7ff8150dd2697358`), two
groups. Every gate whose measured line was converted — `smoke-actiondel`,
`smoke-actionhypo`, `smoke-actionquery`, `smoke-actionread`,
`smoke-actionspawn`, `smoke-fat32-multicluster`, `smoke-poll`, `smoke-runexp`,
`smoke-sendipc` — plus the `sys_writev` gather's blast radius, which is wider
than the diff looks: **every musl-linked program's console output goes through
that path**, so `smoke-shell`, `smoke-compositor`, `smoke-agents`, `smoke-user`,
`smoke-blkmq` and `smoke-nethammer` were re-run although none of them was edited.
`smoke-selftest` is load-bearing again, because this change edits
`GLOBAL_FORBIDDEN` a second time.

`make image` rc=0, zero warnings at `-Werror`; `hygiene_check.sh` ALL SIX;
`kernel.bin` 1,196,426 -> **1,204,618 B** against the 1,572,864 B gate.
`GLOBAL_FORBIDDEN` 75 -> **76**, `[uline] TRUNC` inserted before the final list
line so §NON-NEGOTIABLE 6's terminator did not move.

## 7. What is NOT proven

- **Part (b), the `sys_writev` gather, has no equivalent counter.** `dbg_writes`
  increments once per `writev` regardless of how many iovecs it fans out to, so
  it cannot see the change. The gather's correctness rests on reading
  `__stdio_write` (always two iovecs) and `sys_writev` (one `fd_write_user` per
  iovec) — both quoted above from this tree — plus the regression suite, which
  re-runs every musl-linked gate. It is **not** backed by a measurement of its
  own, and that is a real gap, not a formality.
- **No mutation on part (b).** A mutant reverting the gather would pass every
  gate, because the split it reintroduces is invisible unless a race is won.
  Recorded as uncovered rather than papered over — the same disposition DDR-1031
  took for its uncoverable `invlpg`.
- The `smoke-actiondel` and `smoke-surfclose` CI failures remain **unattributed**
  (see §5). What changed is that the next occurrence is decidable, because the
  gate now dumps what it found.
