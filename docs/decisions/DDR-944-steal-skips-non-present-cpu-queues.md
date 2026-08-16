= DDR-944 — a queue on a non-`present` CPU is never drained and never stolen from

**Status:** HYPOTHESIS with a live discriminator. **No fix.**
**Date:** 2026-08-16
**Lineage:** DDR-934 → DDR-936 → DDR-940 → DDR-941 → DDR-942 → **DDR-944**.

## Everything already excluded, by measurement

| excluded | by |
|---|---|
| allocation of the TCB/stack | `spawned=4/4` (DDR-942) |
| allocation *inside* the worker | `done=0x0`, not `0x0f00` (DDR-942) |
| `sched_unblock` CAS gate | `ubcas=0` (DDR-940) |
| `rq_push` `rq_on` gate | `ubrq=0` (DDR-940) |
| pick-time drop of a non-READY entry | `rqmiss=0` (DDR-942) |
| missing cross-CPU wake IPI | four separate refutations (DDR-934/936/939) |

The blk workers never call `sched_unblock` at all: `sched_create` builds them
`THREAD_READY` and enqueues directly at `sched.c:902-905`. So they *were*
pushed, nothing removed them, and they never ran.

## The mechanism

`sched_create_state` enqueues on the **creating CPU's** queue:

```c
if (initial_state == THREAD_READY) {
    struct percpu *pc = this_cpu();
    rq_push(pc ? (int)pc->cpu_idx : 0, t);      /* sched.c:902-905 */
}
```

A queue is drained by (a) its own CPU running `schedule()`, or (b) another CPU
stealing. And `steal_pass` **skips any CPU that is not `present`**:

```c
struct percpu *pc = percpu_get((unsigned)c);
if (!pc || !pc->present)
    continue;                                    /* sched.c:542-544 */
```

So if a thread is enqueued on `g_rq[c]` where CPU `c` is not (or not yet)
`present`:

- CPU `c` is not scheduling, so it never drains its own queue; **and**
- every other CPU refuses to steal from it.

The thread is stranded permanently, with `ubcas=0`, `ubrq=0`, `rqmiss=0`, and
`done=0x0`. **That is every observation, with nothing left over.**

The reachable route is a creator whose `this_cpu()` returns a `percpu` whose
`cpu_idx` is set while `present` is still 0 — i.e. work created on an AP during
bring-up, before the AP is marked present, or on any CPU whose `present` is
cleared afterwards. `rq_push` itself never consults `present`, so nothing stops
the enqueue.

## Status: HYPOTHESIS, not a finding

This is a mechanism that fits every measurement. It is **not** confirmed, and
per §6.0-B it will not be fixed on fit alone. The tally that earns this caution:
DDR-920, 928, 932 each named a mechanism from inference and each was refuted;
DDR-936's own two gates were excluded by instrument; my "16 gates" claim was
grep output mistaken for evidence; and DDR-942's first-draft `rqdepth` criterion
would have fired on every healthy boot. Fit is not proof.

## Discriminator (already shipped in `e296030`, plus one field here)

`rqdepth`/`rqcpus` are live. This DDR adds `rqpres=` — a bitmask of which CPU
indices hold queue entries, paired with the `present` bitmask — so the question
"are the stranded entries on a non-present CPU?" is read directly.

**Confirms this DDR:** on a failing run, queue entries sit on a CPU index whose
`present` bit is 0, and `rqdepth` stays above baseline and non-decreasing.

**Refutes this DDR:** all entries are on `present` CPUs (then the defect is in
the pick loop of a live CPU, not in reachability), or `rqdepth` returns to
baseline (then the thread left the queue by a path none of five counters
observe, and "still queued" is wrong).

**Reminder (DDR-942):** `rqdepth` must be read as a *series*. Baseline on a
healthy boot is `rqdepth=6 rqcpus=1`; a single nonzero line means nothing.

## A concrete route exists — and `g_rq[]` is not migrated with `g_percpu[]`

`percpu_init_early()` claims slot 0 with `present = 1` because "the scheduler
ticks long before ACPI/APIC init" (`percpu.c:49-57`). Threads created in that
window take `rq_push(0, t)` and land on `g_rq[0]`.

`percpu_init_bsp()` then re-homes the BSP to its real roster index
(`percpu.c:62-84`):

```c
if (ridx != 0) {
    g_percpu[ridx] = g_percpu[0];      /* percpu struct migrates */
    g_percpu[ridx].self = &g_percpu[ridx];
    g_percpu[0].present = 0;           /* slot 0 is now NOT present */
    …
}
```

**`g_rq[]` is a separate array and is NOT migrated.** So when `ridx != 0`:

- threads already queued on `g_rq[0]` stay there;
- the BSP now runs as `ridx` and pops from `g_rq[ridx]`, never `g_rq[0]`;
- `steal_pass` skips `g_rq[0]` forever, because slot 0's `present` is now 0.

Every thread enqueued before `percpu_init_bsp()` is stranded permanently. That
is this DDR's mechanism with a specific trigger, and it is a genuine latent
defect regardless of whether it fires today.

**It is probably NOT our bug.** The code's own comment says "Dormant on QEMU
(BSP roster index is 0), but correctness must not depend on that" — if
`lapic_apic_id_at(0) == lapic_id()` then `ridx == 0` and the migration branch
never runs. Recording it as **latent** rather than causal, because asserting it
without measuring `ridx` would be the same error this investigation has already
made seven times.

`rqq`/`rqpres` decide it directly: **bit 0 set in `rqq` while clear in
`rqpres`** is this exact bug, live. Baseline on a passing boot is
`rqq=1 rqpres=1` — bit 0 queued *and* present, i.e. the dormant case.

## Note on the uniprocessor case

`smoke-agent-click` mode A boots with no `-smp` (DDR-936), so only CPU 0 exists
and this mechanism cannot apply there. If mode A shares a root cause with the
blk workers, it is **not** this one. §6.0-C: they may still be separate defects,
and this DDR claims only the `-smp` probes.
