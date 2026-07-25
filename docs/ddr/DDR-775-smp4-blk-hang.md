# DDR-775 — `-smp 4` boot hang narrowed to the virtio-blk completion wait

**Status:** **investigation / findings — no code this slice.** The subsystem is
now identified with evidence and two structural hazards are documented, but the
exact trigger is **not** yet proven, so no speculative concurrency fix was shipped.
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 3**
(`-smp 4` percpu-sched race root-cause).

## Evidence

| Run | Gate | Mode | Result |
|---|---|---|---|
| 30151522978 | `smoke-surfdestroy` (q35 `-smp 4`) | 180 s timeout | HUNG after `SYSFSTAT OK`; missed the **first** sentinel |
| 30155872016 | `smoke-blk-integrity` (q35 `-smp 4`, **concurrent read data-verify**) | 180 s timeout | missed `[smp] blk integrity OK` |
| local ×3 | `smoke-surfdestroy` | — | **3/3 PASS** |
| 30158060606 | `smoke-blkmq`, `smoke-blk-integrity`, `MSI-X-on-AP`, `smoke-surfdestroy` | `-smp 4` | **ALL PASS** — same commits, run green end-to-end |

| 30163444702 | `smoke-smpuser` — "user-on-AP" (`-smp 4`, ring-3 thread on a non-BSP CPU) | 180 s timeout | missed `[smp] user on AP OK`; **DDR-776 watchdog SILENT — no `[vblk] stuck` line** |

## ⚠ CORRECTION (2026-07-25) — the virtio-blk narrowing below is NOT supported

The DDR-776 watchdog's **negative** result refutes part of this DDR's conclusion.
In run 30163444702 a *third* `-smp 4` gate failed — `smoke-smpuser`, which is
**not** block-I/O — and the watchdog printed **nothing**, i.e. **no virtio-blk
request was stuck for >5 s**. The timer was demonstrably still firing (the boot
progressed through the fuzz test and a ring-3 thread exited), so the watchdog did
run and its silence is meaningful evidence, not absence of instrumentation.

So the failure is **not** (or not only) a stuck block request. What all failures
share is only `-smp 4`; the missed sentinels differ each time:

- `smoke-surfdestroy` — hung after `SYSFSTAT OK` (next sentinel `SYSREAD OK`)
- `smoke-blk-integrity` — missed `[smp] blk integrity OK`
- `smoke-smpuser` — missed `[smp] user on AP OK`, boot still progressing

**Revised position:** the original Section B#3 framing (an SMP/percpu-scheduler
race affecting AP-dependent proofs) is better supported than the virtio-blk
narrowing. The `sys_read`→virtio-blk inference from the surfdestroy stall point
was a reasonable hypothesis from one data point, but it does not generalise, and
this DDR's "Narrowing" section should be read as **superseded** by this note.

**What survives unchanged:** Hazard 1 (the completion wait is unbounded) and
Hazard 2 (single-element `slot_waiter`) are still genuine defects and still S2
violations worth fixing on their own merit — they are simply **not proven to be
this hang's trigger**.

**Therefore the hang is INTERMITTENT, not deterministic.** Run 30158060606 passed
every gate that failed in the two runs above, including `smoke-blk-integrity`
(which failed in 30155872016) and the `MSI-X-on-AP` test that specifically proves
a blk completion running on a non-BSP CPU. This is consistent with a **rare missed
completion**, not systematic breakage — and it means **one green run cannot prove
a fix**. The honest success criterion is the DDR-776 diagnostic firing (naming the
stuck request) or several consecutive green runs.

**Narrowing:** two *different* gates, both `-smp 4`, both **block-I/O-centric**,
both hanging at the full 180 s bound; three consecutive local runs pass. In the
surfdestroy case the next sentinel after the hang point (`SYSFSTAT OK`) is
`SYSREAD OK`, i.e. the boot stalled inside `sys_read` → `vfs_read` → SFS →
**virtio-blk**. So this is *not* a generic "percpu-sched race" as Section B#3
originally framed it: it is the **virtio-blk completion path under SMP**.

It is also **pre-existing and unrelated to the DDR-774 NVMe work**: the same
`smoke-surfdestroy` gate failed in run 29726803735 (DDR-766), and both failing
gates boot with **no NVMe device**, so `nvme_init()` never runs.

## Hazard 1 (confirmed defect) — the completion wait is UNBOUNDED

`kernel/drivers/blk/virtio_blk.c`, `submit()`:

```c
while (!v->req[s].done)
    sched_block_on(&v->compl_lock);        /* no deadline, no escape */
```

If a completion is ever missed for any reason, the submitter blocks **forever**.
This is a direct violation of **Security Invariant S2 ("bounded everything —
every bound returns an error or a clean kill, never a panic/hang")**: there is no
bound here at all. Independently of what the underlying trigger turns out to be,
this is what converts a transient miss into a permanent 180 s boot hang and makes
the failure undiagnosable — the gate can only report "sentinel not found".

**Note the lost-wakeup race itself is correctly handled** and is *not* the defect:
`sched_block_on()` publishes BLOCKED under `compl_lock`, and `complete()` acquires
that same lock before `sched_unblock()` (the DDR-SMP-3c-locks-4 pattern). So the
classic check-then-block window is closed.

## Hazard 2 (latent defect, probably NOT this trigger) — single-element slot waiter

```c
struct tcb *slot_waiter;      /* ONE submitter waiting for a free slot */
...
v->slot_waiter = current_thread;
sched_block_on(&v->compl_lock);
```

With more than one submitter waiting for a request slot, the second **overwrites**
the first, which is then never woken — a permanent lost wakeup. However
`VBLK_NREQ = 8`, so slot starvation requires **>8 concurrent in-flight
submitters**, which 4 vCPUs are unlikely to reach in these gates. Recorded as a
real defect to fix, but **not claimed as the cause of the observed hangs**.

## Why no code in this slice

The two candidate fixes both touch SMP concurrency in the **shared** block path
that every filesystem gate depends on, and — critically — **the bug does not
reproduce locally (3/3 pass)**, so any change would be pushed unvalidated and
judged only by a ~2.5 h CI run. Shipping a speculative concurrency change under
those conditions is how a one-gate flake becomes a many-gate outage. The evidence
gathered here is the prerequisite for fixing it correctly, not a reason to guess.

## Next slice (specified)

1. **Bound the wait (S2 compliance, do this first).** Replace the unbounded
   `sched_block_on` loop with a deadline-bounded wait so a missed completion
   degrades to an I/O error plus a diagnostic (`[vblk] completion timeout slot=N
   lba=…`) instead of a silent hang. Note this needs either a scheduler
   *timed* block (does not exist today — `sched_block_on()` has no timeout) or a
   bounded `g_ticks` yield-loop; **choosing between those is the design decision
   of that slice** and must be made explicitly, since a yield-loop changes the
   "sleep, never spin" property the current design deliberately has.
2. With the diagnostic in place, the next CI failure names the stuck request, at
   which point the real trigger (missed IRQ vs. lost `virtq_pop_used` vs.
   `head2slot` corruption) becomes decidable rather than speculative.
3. Fix Hazard 2 by turning `slot_waiter` into a proper wait list (or by waking all
   waiters and letting each re-check the loop condition — safe, since a spurious
   wake simply re-tests).

## Architecture prerequisite checklist (for that next slice)

- New NSI/syscalls: none. TCB fields: possibly one (a wait deadline) — decide in
  the DDR. PMM/VMM: none. Capabilities: none. AETHER queue/audit: none.
- **Scheduler hooks: YES, likely** — a timed block would be a scheduler feature;
  this is the crux and must be scoped deliberately.
- Filesystem/root-mount: indirectly (every FS gate rides this path).
- New gate: none needed — `smoke-blk-integrity` and `smoke-surfdestroy` already
  exercise it; the win is that failures become *diagnostic* rather than silent.
- **Security invariants: S2** (the unbounded wait is the violation being fixed)
  and **S6** (fault isolation — a driver-level timeout must fail the I/O cleanly,
  never panic). Nothing touches W^X, NX, or capability contracts.
