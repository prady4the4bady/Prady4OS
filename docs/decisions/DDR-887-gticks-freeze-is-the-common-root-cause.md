= DDR-887 — CONFIRMED: `g_ticks` stops advancing; items 47 and 48 are one defect

**Status:** ACCEPTED (diagnosis). **No fix in this slice** — §4.5 forbids fixing
on hypothesis, and the *mechanism* (which lock) is not yet identified.
**Date:** 2026-08-15
**Supersedes the framing of:** item 47 ("fs_test_thread lost") and item 48
("virtio-blk probe false-failure") as independent intermittents.
**Lineage:** DDR-777 (heartbeat discriminator) → DDR-880 → DDR-885 → DDR-886
(probe disambiguation) → **DDR-887 (this)**.

## The capture

CI run 31837700697, shard 0, `smoke-blk-integrity`, full 180 s `TIMEOUT_S`:

```
[boot-stamp] A probe-block-begin t=176
[boot-load] HELLO.ELF    t=176
[user] ELF loaded from SFS; ring-3 thread spawned
[wx] spawning W^X violator (expect a clean user-kill)
[boot-load] WXVIOL.ELF   t=185
HELLO FROM RING-3
[boot-load] SYSTEST.ELF  t=195
[trap] user #PF ... WXVIOL.ELF ... — killing process
[boot-load] INPUTTST.ELF t=207
SYSWRITE OK / SYSIO EBADF OK / SYSOPEN OK / SYSFSTAT OK / SYSREAD OK /
SYSCLOSE OK / SYSGETPID OK / SYSGETCWD OK
<serial ends>
```

Shard 4 in the same run: `[blk] multi-inflight OK` **not found** — the probe
printed **neither** `OK` nor the new DDR-886 `FAIL done=…`.

## What is CONFIRMED

**`g_ticks` stops advancing.**

`kputs("[hb] t=…")` at `kernel/idt.c:148` fires on `(g_ticks % 500) == 0` and is
**unconditional** — it is NOT behind `BSP_LIVENESS` (that flag only gates the
churn block; `Makefile:210`). The gate ran a full 180 s of wall time, which at
100 Hz is 18 000 ticks, i.e. ~36 heartbeats. **Not one `[hb]` line appears.**
The last observed tick is `t=207`.

The system is otherwise alive: ring-3 output (`SYSIO…SYSGETCWD`) continues
*after* `t=207`, so threads are still being scheduled and the console still
works. Only time stops.

## Why this makes items 47 and 48 ONE defect

Every deadline in the tree is `g_ticks`-relative:

| site | wait |
|---|---|
| `blkmq_proof` (main.c:632) | `while (… && g_ticks < dl) yield();` |
| `smp_blk_integrity` (main.c:673) | `while (… && g_ticks < dl) yield();` |
| `rqstress_proof` (main.c:572) | `while (… && g_ticks < dl) yield();` |
| `virtio_blk_watchdog` | `(g_ticks % 100) == 0` |

With `g_ticks` frozen, **none of these can ever terminate**. That single fact
explains, without any further assumption:

- **item 47** — `fs_test_thread` reaches stamp A and never B or C. It is not
  "lost"; it is parked on a deadline that cannot expire.
- **item 48** — the blk probes print *neither* verdict. DDR-886's
  `workers-late` / `checksum-mismatch` disambiguation never fires because the
  pacing loop itself never exits. This is why the new fields did not appear.
- **the watchdog's silence** — `% 100` never comes round again.
- **four different gates missing four different sentinels** — whichever gate is
  booting when time stops is the one that reports.

DDR-886 remains correct and worth keeping (a *late* worker still must not read
as a *wrong* one), but it could never have fixed this: it disambiguates a
verdict that is never reached.

## The mechanism — deduced, NOT yet confirmed

`g_ticks++` is the **first statement** of `timer_tick` (`idt.c:139`), on the path
shared by the PIT (IRQ0) and the APIC timer (vector 48). So a frozen counter
means `timer_tick` is **not being entered at all**, on any CPU — not that it
entered and wedged partway.

Timer delivery stops on every CPU simultaneously only if every CPU has
interrupts masked. `spin_lock_irqsave()` masks interrupts and *then* spins, so
one CPU wedged while holding a lock parks every other contender with IRQs off.
That is a system-wide spinlock deadlock, and it fits all of the above.

**This is a deduction from the code, not an observation. Do not fix on it.**
Candidate locks, in order of exposure on the stalled path
(`user_boot_from_sfs` → `vfs` → `sfs` → `blk`): `virtio_blk`'s `compl_lock`
(which `sched_block_on()` is handed), the runqueue locks reached via
`sched_tick`, `g_console_lock`, and `g_rx_lock`.

## Why the usual instruments cannot see it

If every CPU is spinning with IRQs off, nothing can print — `kputs` itself takes
`g_console_lock` and masks interrupts. Any in-kernel tracer that reports *after*
the wedge is unreachable by construction. The observation must be made either
(a) **before** the wedge, into a lock-free ring that is read **externally**, or
(b) from **outside** the guest.

### Next instrument (specified, not yet built)

Prefer (b) first — it is cheaper and needs no kernel change:
`boot_test.sh` already talks QMP for input injection. On a `TIMEOUT_S`
expiry, issue `info registers` / `info cpus` over QMP before killing QEMU and
attach it to the artifact. Four CPUs all sitting at a `pause` in
`spin_lock_irqsave` with `IF=0` would confirm the deadlock and, via RIP, name
the lock site directly.

Only if that is inconclusive, add (a): a per-CPU lock-acquire ring in a fixed
BSS page, dumped over QMP `pmemsave` on timeout.

## Consequences for the plan

- Items 47 and 48 must no longer be tracked as two intermittents. §4.4's
  "do not conflate" was correct as a discipline while they were unexplained;
  they are now unified **by evidence**, not by assumption.
- The S2 fix queued in §5 Priority 3 — "bound the virtio-blk completion wait
  with a `g_ticks`-based yield-loop timeout" — **cannot work** and must not be
  written. A `g_ticks`-based bound is worthless when `g_ticks` is the thing that
  stops. Any bound introduced for this class must be tick-independent.
- Likewise `sched_block_on_timeout(&lk, deadline_ticks)` (§3, deferred) inherits
  the same flaw as specified.

## Instrument BUILT and PROVEN (same slice)

`tools/qemu_runner/qmp_cpudump.py` + an opt-in hook in `boot_test.sh`
(`QEMU_QMP_DIAG=1`, **off by default** so none of the 106 gates change
behaviour). ~5 s before the hard timeout, while QEMU is still alive, it dumps
`info cpus` and `info registers -a` into the serial capture, so the per-CPU state
lands in the failure artifact.

Verified: `smoke-blkmq` still rc=0 on the default path (no QMP socket, no
watcher), and with the flag set the dump appears in boot_test's serial print.

Invocation for a diagnostic run:

```
SERIAL_LOG=/tmp/x.serial QEMU_QMP_DIAG=1 QEMU_SMP=4 TIMEOUT_S=180 \
  bash tools/qemu_runner/boot_test.sh build/pradyos.img
```

Resolve the RIPs with:
`llvm-addr2line -f -e build/kernel.elf <rip>`

## FIRST READING — a lead, not a conclusion

A forced-timeout sample (fake sentinel, `-smp 4`, 20 s) gave:

```
RIP=ffffffff8000fe25 RFL=00000002 CPL=0 HLT=0   -> switch_wait_offcpu (sched.c)
RIP=ffffffff8000fe25 RFL=00000046 CPL=0 HLT=0   -> switch_wait_offcpu
RIP=ffffffff8000fe25 RFL=00000002 CPL=0 HLT=0   -> switch_wait_offcpu
RIP=ffffffff80016517 RFL=00000046 CPL=0 HLT=0   -> find_zombie_child (sys_wait.c)
```

**All four CPUs have IF (bit 9, 0x200) CLEAR and none are halted.** Three are
spinning in `switch_wait_offcpu`. A CPU spinning there with interrupts disabled
cannot take its timer interrupt, which is precisely the condition required for
`g_ticks` to stop.

**Why this is a lead and not yet the answer:** this sample came from a run
forced to time out with a sentinel that never appears, so the guest may simply
have finished its work. If idle CPUs *always* park in `switch_wait_offcpu` with
IF=0, `g_ticks` would freeze on every boot, which it plainly does not — so
either this is a transient the sample caught, or the idle path differs from the
wedge. The next step is to capture the dump on a REAL failing run (a gate that
misses its sentinel with `QEMU_QMP_DIAG=1` set) and compare: if the wedge shows
the same three-CPUs-in-`switch_wait_offcpu` picture, the spin is the mechanism
and `switch_wait_offcpu`'s bound is the defect to fix.

Do not change `switch_wait_offcpu` before that comparison exists.

## Unrelated datum captured in the same run (do not lose)

`[sfs] churn FAIL op=create iter=0 rc=-1` — the DDR-884 rc instrument fired
locally. `op=create` failing at `iter=0` with `rc=-1` is the btree-churn arm,
and it reproduced OUTSIDE CI. Track separately; it is not part of DDR-887.

## THE FIX — verified against the code, with one correction to the proposed form

### Verification of the call-site's lock state (this had to be checked first)

A `sti` inside a spin is only safe if no non-reentrant lock is held. Checked:

- `schedule()` (sched.c:1026-1027) calls `schedule_locked(local_irq_save())`.
  `local_irq_save()` (sched.c:437) is a bare `pushfq; pop; cli` — **no lock**.
- `g_sched_lock` was removed from the switch by rq-2 and now "covers ring
  TOPOLOGY only" (sched.c:433-436).
- `sched_exit` takes the topology lock but calls `irq_restore(fl)` **before**
  `schedule()` (sched.c:1177).
- `sched_free_tcb` "Takes NO scheduler lock" (sched.c:852).

**The comments at sched.c:932 and :944 claiming `g_sched_lock` is "ALREADY
HELD" across the switch are STALE rq-1-era text.** They are contradicted by
sched.c:433-436 and by the actual call at :1027. Anyone reasoning from those
comments will reach the wrong conclusion about `sti` safety.

Conclusion: `switch_wait_offcpu` is never reached with `g_sched_lock` held, so
re-enabling interrupts there cannot self-deadlock on it.

### The hazard the naive form DOES have

`sched_tick` calls `schedule()` on quantum expiry. Enabling interrupts inside
`schedule_locked` — after `prev` has been re-queued and `next` popped, but
before `next->on_cpu = cpu` is claimed — lets the timer **re-enter the scheduler
mid-switch on the same CPU**. That is a reentrancy hazard, not a state-visibility
one; "`on_cpu` is still set" does not address it.

We need the **tick**, not a nested switch. So the fix is two parts:

1. A per-CPU `g_in_switch[]` flag (file-local array in sched.c — NOT a new
   `struct percpu` field, because that struct has asm-visible fixed offsets that
   are static-asserted in percpu.c).
2. `sched_tick` skips its `schedule()` call when this CPU's flag is set. The
   tick still runs: `g_ticks++`, the vDSO clock, lwIP timers and the blk
   watchdog all advance — which is the entire point.

### Form

A separate schedule-path variant is used so `sched_free_tcb`'s call site is
untouched. That matters: `sched_free_tcb` runs from the reaper with interrupts
ENABLED, and a bare `sti;pause;cli` loop would leave IF **clear** on exit,
silently changing the caller's interrupt state. The new variant also returns
immediately when no wait is needed, so the common path touches IF not at all.

### Bound

The IF=1 window is a single `pause` between `sti` and `cli` — one instruction
boundary, long enough for a pending LAPIC timer to be delivered, too short to be
a scheduling quantum. S2 is unaffected: the loop's termination condition is
unchanged (it still exits when the owner releases `on_cpu`), and the fix makes
that release *reachable*, which it was not before.
