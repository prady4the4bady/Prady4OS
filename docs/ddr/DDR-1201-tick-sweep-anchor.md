# DDR-1201 — #115: the tick's timed-wake sweep must start on the ring

**Status:** implemented in this PR. A kernel change (the scheduler).
**Decision:** the operator, #45 comment 6761846242 item 1: option (a) of #115, anchor the walk on `idle0`; audit the other ring walks; add a gate arm with a mutant shown red.
**Tree:** every reading below is at `97f0397` (`dev/phase1`), before this change.
**Honesty labels:** QEMU TCG on GitHub-hosted runners only. Nothing here is verified on real hardware. §8 holds the run links.

---

## 1. The defect (issue #115; first written down in DDR-1200 §5.3)

`sched_tick` runs in the timer ISR on every CPU. Its timed-wake sweep (DDR-955) took `g_sched_lock` and walked the all-threads ring like this:

```c
struct tcb *_t = current_thread;
do { ...; _t = _t->next; } while (_t != current_thread);
```

The walk ends only when it meets its start node again. So the start node must be on the ring. `current_thread` is not always on it:

1. `sched_exit_common` stores `THREAD_ZOMBIE`, releases `g_sched_lock` and calls `schedule()` (`sched.c:2786-2798`). The thread is still `current_thread` on its CPU.
2. Its last switch can wait in `switch_wait_offcpu_sched` (`sched.c:1822`), which spins on `sti; pause; cli` (`sched.c:737`). `current_thread` is changed only later (`sched.c:1857`). **So a timer tick can land with `current_thread` == the zombie.**
   - There is a second, shorter window: between the lock release at `:2797` and the interrupt mask inside `schedule()`, when the caller had interrupts on.
3. Meanwhile a collector on another CPU can unlink the zombie. Both collectors unlink first and wait for the zombie to leave its CPU only afterwards, in `sched_free_tcb` (`sched.c:1484`):
   - `reaper_thread` (`sched.c:2991`);
   - `sched_destroy` (`sched.c:1685-1692`), which `sys_wait4` calls (`sys_wait.c:66`).
4. `sched_ring_unlink` leaves the unlinked node's `->next` as it was (`sched.c:1468-1473`). A walk from the zombie therefore enters the live ring and never meets the zombie again.
   - The loop runs in the timer ISR, with IF clear, holding `g_sched_lock`. Every other CPU stops at its next tick, in `irq_save()`.

**Before this PR:** found by reading, not reproduced. **In this PR:** the state of steps 1-3 is produced on purpose in a test build, and the old walk is shown not to come back (§5, §8).

## 2. The fix

```c
struct tcb *_a = &idle0;
for (struct tcb *_p = idle0.next; _p != &idle0; _p = _p->next)
    if (_p == current_thread) { _a = _p; break; }
struct tcb *_t = _a;
do { ...; _t = _t->next; } while (_t != _a);
```

- **The walk now ends on every tick.** One pass from `idle0`, under the lock, says whether `current_thread` is on the ring. The sweep then starts on a node that is known to be on it.
- **`idle0` is always on the ring:**
  - it is a static object (`sched.c:90`) and is the first node (`idle->next = idle`, `sched.c:1056`), linked before any CPU has a `current_thread`;
  - only two things unlink a node. The reaper takes only a `THREAD_ZOMBIE`, and an idle thread never becomes one. `sched_destroy` is called on a thread its caller created, or on a zombie child found by pid (`sched.c:1534-1542` already lists this);
  - under `g_sched_lock` the ring does not change, so both loops meet `idle0` again.
- **Nothing else about the sweep changes.**
  - When `current_thread` is on the ring, which is every case but the one in §1, the sweep starts there exactly as before. It takes the same waiters, in the same order.
  - That matters because the sweep takes at most 32 waiters. With a fixed start it would always prefer the waiters that follow `idle0`. I did not want this fix to change who is woken first. DDR-1200's RCU sweep removes the cap, and starts at `idle0`.
  - When `current_thread` is off the ring, the sweep starts at `idle0`.
- **Cost, not measured:** one more pass over part of the ring per tick, under the lock. It reads only `->next`, in nodes the sweep reads anyway, and stops at `current_thread`.
- **What the fix does not do:** a zombie can still be unlinked while it is on its CPU. That is safe as long as nothing walks from it, which §3 checks.

## 3. Audit: every ring walk that starts at `current_thread`

All ring walks are in `kernel/proc/sched.c`. No other file follows `tcb->next` (checked by grep over every file that names `struct tcb`).

| # | Walk (`sched.c`) | Starts at | Lock | Who runs it | Can the start be off the ring? |
|---|---|---|---|---|---|
| 1 | `sched_tick` timed-wake sweep (`:2189-2310`) | `current_thread` | `g_sched_lock` | timer ISR, every CPU (`idt.c:377`, `:780`) | **Yes: #115. Fixed here.** |
| 2 | `wake_wait4_locked` (`:1580`) | `current_thread` | caller's | `sched_exit_common` (`:2796`); `sched_unclaim_child` (wait4) | No in product paths. Yes through the probe below. |
| 3 | `sched_wait_child` (`:1598`) | `self->next`, ends at `self` | yes | wait4 syscall | No. It also has a 1024-node bound. |
| 4 | `sched_snapshot` (`:1661`) | `current_thread` | yes | `SYS_GETPROCS` | No |
| 5 | `find_any_pid_locked` (`:2666`) | `current_thread` | caller's | `sched_exit_common` (`:2775`) | As row 2 |
| 6 | group-kill walk (`:2713`) | `self->next`, ends at `self` | yes | `sched_exit_common` | As row 2 |
| 7 | reparent walk (`:2764`) | `current_thread->next` | yes | `sched_exit_common` | As row 2 |
| 8 | `sched_find_pid` (`:2809`) | `current_thread` | **none** | boot tests in `main.c` only (`:2327`, `:2949`, `:3657`) | No. It is an unlocked reader, which is a different defect class (DDR-1157 moved the syscall paths off it). Not changed. |
| 9 | `find_pid_locked` (`:2826`) | `current_thread` | caller's | checkpoint and resume syscalls | No |
| 10 | `find_user_pid_locked` (`:2879`), `find_user_group_live_locked` (`:2895`) | `current_thread` | caller's | kill, agent metrics, rollback syscalls; `sched_exit_common` (`:2781`) | No in product paths |
| 11 | `pid_alive` (`:2959`) | `current_thread` | caller's | `reaper_thread` | No: the reaper never exits |
| 12 | `reaper_thread` scan (`:2980`) | `current_thread->next` | yes | the reaper | No |
| 13 | `sched_trace_dump` (`:3089`) | `current_thread` | yes | a `main.c` probe | No |

**Why "No" holds in product paths.**
- A thread leaves the ring in three ways only:
  - as a zombie, by the reaper or by wait4's `sched_destroy`;
  - as a thread that never ran, destroyed by its creator (the create error paths at `sched.c:1391`, `:1436`, `:1453`, and four `main.c` sites that say "never ran: still BLOCKED");
  - in `sched_rqfree_probe` (below).
- A thread becomes a zombie only by its own `sched_exit_common`. Its walks (rows 2, 5, 6, 7, 10) all run **before** the lock release at `:2797`, in the same lock hold as the `THREAD_ZOMBIE` store or an earlier one. Nobody can unlink it before that release.
- After the release, the zombie runs only `schedule()`, which walks no ring (rq-1 removed that walk), and the interrupt handlers of its CPU. Of those, only the timer's `sched_tick` walks the ring.
- Every other walk is run by a live thread in a syscall or by a kernel thread that never exits.

**So in product paths, row 1 is the only affected walk.**

**The one other path: `sched_rqfree_probe` (`smoke-rqfree` only; by reading, not reproduced; issue #116).**
- The probe makes a victim runnable and destroys it at once (`sched.c:3038-3039`). Another CPU may steal and run the victim in between; the probe's comment says so.
- A stolen victim is then **running and off the ring**. Two things follow:
  - a tick on its CPU starts row 1 off the ring. This fix covers that, because it does not depend on who unlinked the node;
  - the victim's own `sched_exit(0)` runs row 2 (always) and row 7 (once init exists) from an unlinked node, under `g_sched_lock`. Those never end.
- **Not changed here.** The operator's decision covers the tick walk, and this is a test path. It is tracked as #116, with the options. Not attributed to any recorded failure.

## 4. What the default build gains

- `kernel.bin` stays at 1,634,698 B in a local build (it was 1,634,698 B at `97f0397`; the image is padded, so only the hash moved). CI's number is in §8.
- The test code of §5 is under `#if ZANCHOR_TEST`, which is 0 in every shipped build and in every shard.

## 5. Gate: `smoke-zanchor`

**The problem with testing this.** The real window is a few instructions wide. A gate that waits for it to happen by chance would pass on the unfixed tree almost every time. So the window is held open in a test build.

**Build flag `ZANCHOR_TEST=1`** (default 0). It adds three hooks and a probe. The hooks count; they repair nothing.
- **The hold** (`za_hold`, called at the top of `schedule_locked` when `prev` is the victim and a zombie). It sets `g_in_switch` as the product's wait does, and spins with interrupts opened once per turn. It keeps spinning until a collector has unlinked the victim **and** 3 ticks have landed on this CPU after that.
  - **No lock is held there.** Since rq-2, `schedule()` enters `schedule_locked` with `local_irq_save()` only (`sched.c:2161-2163`); the comment above `schedule_locked` that says `g_sched_lock` is held is out of date. So the collector can take `g_sched_lock` while the victim holds.
  - It runs before the pick, not at `:1822`. Otherwise the thread it had just popped from the runqueue would wait in its hands, and that thread may be the collector.
  - **Its window is `sti; nop; pause; cli`, not the product's `sti; pause; cli`** (§6, first item). The first CI run used the product idiom and hung.
  - It gives up, as inconclusive, after 400 ticks of this CPU, or 400 ticks of the BSP, or 200,000,000 spins. A round that times out stops the probe (`aborted=1`), because the hooks follow one victim at a time.
- **The unlink mark:** `sched_ring_unlink` sets `g_za_unlinked` when it unlinks the victim. Its callers hold `g_sched_lock`.
- **The hit count:** `sched_tick`, under `g_sched_lock`, counts a hit when `current_thread` is the victim and `g_za_unlinked` is set. Both are read under the lock, so a hit is exact: at that tick the victim was current on this CPU and was off the ring.
- **The escape count:** the sweep counts and leaves if it has not met its start after `ZANCHOR_RING_MAX` nodes (4096; the ring holds fewer than 100 threads at that point of boot). This is an instrument for the mutant, not a fix. `ZANCHOR_RING_MAX=0` removes the bound.

**The probe** (`sched_zanchor_probe`, run first in `fs_test_thread`, before init exists and while the other CPUs are idle). 8 rounds per unlink path. Each round makes one kernel thread call `sched_exit(0)`:
- **reaper path:** the victim is an orphan (`parent_pid` 0), so `reaper_thread` unlinks it;
- **`sched_destroy` path:** the probe claims the victim first, as `sched_wait_child` does, and then calls `sched_destroy`, the function `sys_wait4` calls.

**Arms.**

| Arm | Required line | What it proves |
|---|---|---|
| **A** the state was produced | `PRADYOS_ZANCHOR_A reap_hit=8 destroy_hit=8` | In all 16 rounds, at least 3 ticks landed on a zombie that was still current on its CPU and already unlinked. Both unlink paths. |
| **B** the sweep ended | `PRADYOS_ZANCHOR_B escaped=0` | No tick sweep ran past 4096 nodes. Printed **only if arm A held**; otherwise `ZANCHOR FAIL: ... arm B would be vacuous`. |

A data line goes with them: `[zanchor] cpus=<n> rounds=8 reap_held=<n> destroy_held=<n> hits=<n> escaped=<n> aborted=<n> ring_max=<n>`.

**Would each arm pass on the unfixed tree?**
- Arm A does not depend on the fix. It fails if the window is not forced, which is its job.
- Arm B: on the unfixed tree every counted hit is a sweep that starts on the unlinked victim. It cannot meet its start, so it runs past the bound and `escaped` is above 0. Arm B is then not printed.

**Mutants.** Each must be red in CI on its own arm.

| Mutant | Change | Expected |
|---|---|---|
| **Z1** | the sweep starts at `current_thread` again, with no membership pass (the code before this PR) | arm B red (`escaped` > 0); arm A green |
| **Z2** | `za_hold` returns at once | arm A red (`reap_hit`, `destroy_hit` below 8); arm B withheld |
| **Z1u** | Z1 with `ZANCHOR_RING_MAX=0` | no bound, so this is the defect itself: the boot must stop, with neither line printed |

Z1u is not a gate mutant. It is one run that shows what #115 does without the instrument.

**Why its own CI job.** Every shard asserts, before and after its gates, that `kernel.bin` is the binary the build job published. A gate that rebuilds the kernel cannot run there. `smoke-zanchor` is excluded from the shards with that reason (`tools/ci/shard_check.sh`) and runs in a new `flag-gates` job (`.github/workflows/ci.yml`). The job starts from the same build tree, runs the one gate, and then checks that the default kernel is back with the published hash.
- `KCFLAGS` is not a prerequisite of `$(KERNEL_BIN)`, so the recipe touches `kernel/proc/sched.c` to force the rebuild, as the Makefile's own note says to.
- Both required lines exist only in the flagged build. A stale default kernel fails the gate.

**Not gated.**
- The real, narrow window in the default kernel. No gate waits for it.
- The second window of §1 step 2 (between the lock release and the mask). The hold covers the first.
- The sweep's start when `current_thread` is on the ring ("same order as before"). It is the old loop unchanged, by reading.
- A single-CPU boot: the unlink must come from another CPU. The probe prints `ZANCHOR FAIL: needs 2 CPUs` there.

## 6. Found on the way

- **Under QEMU TCG, `sti; pause; cli` let almost no timer tick in** (measured: run [42824823328](https://github.com/prady4the4bady/Prady4OS/actions/runs/42824823328), job `flag-gates`, head `29047d7`).
  - Two CPUs span on that idiom in `za_hold` for about 100 s. `[apfreeze]` reported one frozen at `ticks=857` and the other moving from 55 to 57. Both RIPs resolve to the `cli` of that loop (`za_hold+0xd8`, against a local build of the same commit with the same size).
  - The likely reason, by reading and not verified in QEMU's source here: `sti` keeps interrupts off for one more instruction, and TCG's `pause` leaves the instruction loop before that hold is dropped, so `cli` runs first.
  - **What it means for the product.** `switch_wait_offcpu_sched` uses the same idiom so that "ticks keep advancing while we wait" (DDR-887). Under TCG they mostly do not. That wait is bounded at 4096 spins, so this is not a hang. It also means the first window of §1 is rarely open under QEMU and should be open on every turn on real hardware. **Not verified on real hardware.**
  - Not changed here. The test hold uses `sti; nop; pause; cli`.
- **#116:** `sched_rqfree_probe` can unlink a running thread (§3). By reading.
- **#117:** `smoke-rqstress-liveness` and `smoke-fs-liveness` never build the kernel with `BSP_LIVENESS=1`. Their `rm -f $(BUILD_DIR)/...` removes files under `build/toolchain`, but the kernel is in `build/`. Measured with `make -n`. Both gates boot the default kernel and still pass.
- The debug-only comment in the sweep that calls it an unlocked walk (`sched.c:2197-2203`) is still wrong, as DDR-1200 §1.1 records. Not touched here.

## 7. What this record does not claim

- That #115 ever happened in a default kernel. §8 shows it in a test build that holds the window open.
- That it explains OPEN-2, the shard 8 "CPU stops ticking" failure, or any other recorded stall. A matching shape is not an attribution (DDR-1056).
- How wide the real window is, or how often a tick lands in it. Not measured.
- Anything about real hardware.

## 8. Evidence

Filled in from CI before the merge.
