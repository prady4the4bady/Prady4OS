# DDR-1195: futex follow-ups owed from #81 — a timeout that can end early, FF/FG without a blocked witness, a stale lock comment

Status: **design**. The code follows in the same PR. Platform: QEMU (TCG, 4 vCPU), `ubuntu-24.04`. Nothing here is verified on real hardware.
Owed from: #81 review (CodeRabbit thread 4435326775: one Major and one Minor finding; both deferred to a follow-up with the operator's knowledge), and my own note on `sched.c:880-883`.
Base: #101 head `d35da2e`. This PR edits the same `user/threadtest.c` and the same `smoke-thread` sentinel line, so it stacks on #101 and takes `dev/phase1` in once #101 merges.

## 1. A futex timeout can end up to one tick early (kernel defect)

**The claim.** DDR-1181 A1.1 says the timeout is "rounded **up** to 10 ms ticks (100 Hz), so a wait never ends early".

**The mechanism that breaks it.**
- `sys_futex` converts the timeout to `ticks = ceil(ns / 10 ms)` (`futex.c:193-194`).
- `sched_block_timeout` sets `block_deadline = g_ticks + ticks` (`sched.c:2553`), and the wait ends at the first tick where `g_ticks >= block_deadline` (`sched.c:2245`).
- `g_ticks` counts tick **edges**, not elapsed time. If the call lands δ before the next edge, the first counted tick arrives after only δ.
- So the real sleep is `(ticks − 1) × 10 ms + δ`, with `0 < δ ≤ 10 ms`. A 50 ms request (`ticks = 5`) can therefore end after a little over 40 ms.

**Why the gate did not see it.** FC measures with the vDSO clock, which advances in 10 ms steps on the same tick. Measured that way, every early wake looks like a whole number of ticks. FC also accepts `cms >= 40`, which passes a wake one tick early.

**The fix.** When `ticks > 0`, wait one more tick: `ticks + 1`. The sleep is then `ticks × 10 ms + δ`, which is at least the request. A zero timeout stays as it is (check the word, return `-ETIMEDOUT`, no sleep), as A1.1 specifies. This is the same choice Linux makes when it rounds a relative timeout to jiffies. **Cost:** up to one extra tick (10 ms) of latency on every timed wait.

## 2. FC measured below tick resolution

The vDSO clock cannot see a sub-tick early wake (§1), so FC changes how it measures:

1. **Calibrate.** The child counts `rdtsc` across three 100 ms vDSO windows, each starting and stopping on a tick edge, and takes the **median** count per ms. Ring 3 may run `rdtsc`: CR4.TSD is not set, and `user/benchtest.c` already uses it.
   - A preemption between an edge and its `rdtsc` read skews one window by up to one slice. The median throws one bad window away.
   - With a single window, a skew that overestimates the rate would make a correct kernel look early, and FC would go red on good code.
2. **Place the call just before an edge.** The child waits for a vDSO edge, then spins on the TSC for 9.5 ms, so the `FUTEX_WAIT` starts about 0.5 ms before the next edge. That is the worst case of §1.
3. **Request 55 ms** (`ticks = 6`) and require the TSC-measured sleep to be **≥ 55 ms**: it must not be early. The sentinel stays `PRADYOS_FUTEX_C timedout=-110 slept_ok=1`. `slept_ok` now means "not early", measured; it no longer means "at least 40 ms".

Expected values, with δ ≈ 0.5 ms and L = wake-to-run latency:

| Kernel | Sleep | Against 55 ms |
|---|---|---|
| fixed (`ticks + 1` = 7) | 6 × 10 + δ + L ≈ 60.5 + L | passes, with a 9 % margin for calibration error |
| without the fix (6) | 5 × 10 + δ + L ≈ 50.5 + L | fails while L < 4.5 ms |

The child prints `[thread] C ms_x100=<sleep in hundredths of a ms> tsc_per_ms=<n>`, so every run shows both margins.

**Vacuity check, before writing the arm:**
- **The unfixed tree turns FC red** whenever L < 4.5 ms.
- **The mutant can stay green** if L ≥ 4.5 ms on a loaded runner. If that happens, I will report FC as not shown red for F2-9 and redesign the arm. I will not lower the 55 ms bound below the request.

**Mutant F2-9 (new):** drop the `+1` in `sys_futex`. It should be red on FC with `slept_ok=0`.
**F2-7** (the timeout is ignored) stays red on FC: the call never returns `-ETIMEDOUT`, so `timedout=1`.

## 3. FF and FG must prove the waiter was asleep (CodeRabbit Major)

**The gap.**
- The FF/FG child sets `ready`, then calls `FUTEX_WAIT`. The parent waits for `ready` and then sleeps 100 ms on the assumption that the child is asleep (`threadtest.c` "the child is asleep").
- If `FUTEX_WAIT` returned at once, the child would exit before the wake and the kill:
  - the parent's wakes would find no waiter, so `private_isolated=1`;
  - reaping the already-dead child would give `killed_waiter=1`.
- So both arms can pass without testing a blocked waiter.

**The fix:**
- The child stores its `FUTEX_WAIT` result in a shared word that starts at 99 (`frc_f`). The value 99 means the call has not returned.
- The parent polls `SYS_GETPROCS` until the child's pid shows `THREAD_BLOCKED` (the `sy_blocked` helper from DDR-1193 Amendment 1 item 3). It checks this **before** the private wakes, and again **after** them together with `frc_f == 99`. So the wakes demonstrably met a sleeping waiter, and they did not release it.
- Then:
  - **FF** prints `private_isolated=1` only if the wakes returned 0 **and** the child is still blocked with `frc_f == 99`. The sentinel becomes `PRADYOS_FUTEX_F private_isolated=1 asleep=1`.
  - **FG** prints `killed_waiter=1` only if the child was blocked with `frc_f == 99` at the kill, **and** the kill reaped it. The sentinel stays the same.

**Vacuity check:** a tree whose `FUTEX_WAIT` returns at once now prints `asleep=0`, and FG then reports `killed_waiter=0`. That is the gap above, now red.

**Mutants:** F2-4 (PRIVATE flag ignored) and F2-5 (`sig_raise` does not wake a futex waiter) are re-run against the new arms. Each must again be red on FF or FG respectively, as CodeRabbit asked.

## 4. The stale lock comment at `sched.c:877-883`

The comment says "schedule() holds it ACROSS context_switch". That stopped being true at rq-2: the comment just below it (`sched.c:891`) says `schedule()` no longer takes `g_sched_lock`, and the #81 lock-order answer relies on that. The comment is rewritten to say what `irq_save` guards now: the ring insert and the topology paths. No code changes.

## 5. Amendments to other records (same PR)

- **DDR-1181 A1.1:**
  - "rounded **up** to 10 ms ticks …, so a wait never ends early" becomes "rounded up to 10 ms ticks, **plus one tick** when non-zero, so a wait never ends early (DDR-1195 §1)".
  - The FC row in A1.4 gets the 55 ms TSC check.
- **DDR-1181 A1.4 sentinel table:** FF gains `asleep=1`.

## 6. Honesty labels

- Futex timeout "never early": will be **gated by FC (TSC, worst-case placement)**, QEMU only. It depends on TCG's TSC staying in step with the PIT over about 360 ms. **Not verified on real hardware.**
- FF and FG: will be gated with a witness that the waiter was blocked.
- Cost of the fix: up to 10 ms extra latency per timed wait. Measured as `ms_x100`; nothing else is claimed.

## 7. Runs (smoke-thread, shard 2; QEMU TCG, 4 vCPUs, `ubuntu-24.04`)

The fixed tree and the mutants were built on head `b094260`, which stacks on #101 `d35da2e`. Each mutant changes one file, builds with 0 warnings, and gives its own `kernel.bin`.

| Tree | Run | Verdict | Measured |
|---|---|---|---|
| fixed `b094260` | [41392392306](https://github.com/prady4the4bady/Prady4OS/actions/runs/41392392306) | green | `[thread] C ms_x100=6085` (60.85 ms for a 55 ms request; predicted ≈ 60.5 ms + L); FF `asleep=1`; FG `killed_waiter=1` |
| F2-9 no extra tick (`futex.c`) | [41392431402](https://github.com/prady4the4bady/Prady4OS/actions/runs/41392431402) | **red on FC only** | `slept_ok=0`, `ms_x100=5071` (50.71 ms; predicted ≈ 50.5 ms + L, so L ≈ 0.2 ms); FF and FG green |
| F2-4 PRIVATE flag ignored (`futex.c`) | [41392434494](https://github.com/prady4the4bady/Prady4OS/actions/runs/41392434494) | **red on FF** | `private_isolated=0 asleep=0`: the cross-process wake reached the waiter. FG reads `killed_waiter=0` because it counts a kill only on a waiter confirmed asleep |
| F2-5 no `futex_kick` (`signal.c`) | [41396059384](https://github.com/prady4the4bady/Prady4OS/actions/runs/41396059384) | **red on FG only** | `killed_waiter=0`, then `THREAD FAIL: killed child never reaped`; FF green |

**What the runs show.** §1 is measured, not inferred: without the fix, a 55 ms timeout ended after 50.71 ms. **TCG only. Not verified on real hardware.**
