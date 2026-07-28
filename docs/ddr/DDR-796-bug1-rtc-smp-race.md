# DDR-796 — BUG-1 root cause: the CMOS/RTC read is not SMP-safe

**Status:** diagnosed, fixed in this slice.
**Date:** 2026-07-28
**Closes:** BUG-1 (intermittent `-smp 4` gate failures), open since DDR-775.
**Supersedes the working hypotheses in:** DDR-777, DDR-791 (both were wrong —
see "What this was not").

## How it was finally reproduced

BUG-1 never reproduced locally in four attempts, because every local attempt
used the wrong recipe. The DDR-791 harness fix (`GLOBAL_FORBIDDEN`) exposed the
missing ingredient by failing `smoke-smp` on 4/4 consecutive CI runs, and the
difference between the gate that passes and the gate that fails is exact:

| gate | CPUs | window | result |
|---|---|---|---|
| `smoke-agentmetrics` | 1 | 150 s | passes |
| `smoke-smp` | **4** | 120 s | fails |

Reproduced locally first try with `-smp 4` **and** a full-length window (a gate
declaring `FORBIDDEN_SENTINEL`, so DDR-785 early exit does not cut the boot
short). Prior attempts had one or the other, never both.

## The evidence that redirected the diagnosis

The obvious reading of `AGENT_METRICS FAIL: agent never observed as scheduled`
is that the agent is never scheduled. The byte offsets in the serial capture say
otherwise:

```
offset 21930 : AGENT_METRICS FAIL: agent never observed as scheduled
offset 97238 : PRADYOS_AETHER_CFG_OK
offset 97581 : aetherd: spawned agent PID=82
offset 97611 : aetherd: reaped PID=82 exit=0
```

**The probe gave up ~75 KB of output BEFORE the agent was spawned at all.** The
agent then spawned, ran and exited normally. Nothing was starved.

So the failure is not in scheduling. It is in the probe's *clock*:

```c
static long elapsed(long start) {
    long now = nsi(SYS_CLOCK, 0, 0, 0);
    if (now < start) now += 86400;   /* midnight wrap */
    return now - start;
}
while (elapsed(start) < 120) { ... }
```

A `SYS_CLOCK` reading that comes back **smaller** than the previous one trips the
midnight-wrap correction, adds 86400, and makes `elapsed` enormous. The 120-second
window collapses to nothing and the probe prints FAIL immediately.

## Root cause

`SYS_CLOCK` → `rtc_now()` → `cmos_read()`:

```c
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);      /* port 0x70: select a register */
    return inb(CMOS_DATA);      /* port 0x71: read it           */
}
```

CMOS access is a **two-port sequence with shared global state** — the index
latched into port 0x70 belongs to the whole chipset, not to the calling CPU.
There is no lock anywhere in `kernel/drivers/rtc/rtc.c`.

Under `-smp 4`, two CPUs interleave:

```
CPU A: outb(0x70, 0x00)   /* select seconds */
CPU B: outb(0x70, 0x04)   /* select hours   */
CPU A: inb(0x71)          /* reads HOURS, believing it is seconds */
```

`rtc_now`'s existing "read twice until two readings agree" loop does **not**
protect against this. That loop guards against an RTC *update-in-progress*
(status A bit 7) — a different hazard entirely. Two racing CPUs can produce two
consecutive agreeing-but-wrong readings, because both readings are wrong in the
same way while the interleaving persists.

The result is a wall-clock that can jump forward or backward arbitrarily. Any
consumer that compares two readings — the metrics probe here, `date`, ambiance
selection (DDR-709), FAT timestamps — can observe time going backwards.

## Why this presented the way it did

* **`-smp 4` only** — a single CPU cannot interleave with itself; the index and
  data accesses are never separated by another CPU's `outb`.
* **Intermittent** — it needs the two `outb`/`inb` pairs to interleave, which
  depends on scheduling luck and host load. A loaded CI runner hits it far more
  often than an idle laptop, which is exactly the observed distribution.
* **A different gate each run** — whichever gate happens to be running a
  clock-reading consumer when the interleave occurs is the one that fails. That
  is the "different gate every time" signature that made BUG-1 look like a
  scheduler defect for so long.
* **Only in full-window gates** — an early-exiting gate usually stops before the
  probe's window logic matters.

## What this was not

Recorded because two plausible hypotheses consumed real effort, and the next
person should not re-run them:

* **Not a scheduler starvation bug** (DDR-791 finding 2). The agent is spawned,
  dispatched, and reaped normally; the probe simply was not watching by then.
* **Not the serial flooding** (DDR-791 finding 1). The 83%-binary console is a
  real and separate defect — still unexplained, still worth fixing — but it does
  not cause this. The reproduction above shows the failure with the flood
  present and the timeline proving the probe exited early for clock reasons.
* **Not AP scheduling / D-08 monitor registration.** The D-08 monitor is a
  host-side Python component and is not involved in this kernel path at all.

## The fix

One spinlock (`g_rtc_lock`), IRQ-saving, held across the **whole** of
`rtc_now()` — not per-`cmos_read()`.

Per-call locking would be wrong: it would make each individual index/data pair
atomic while still letting another CPU change the index between the *paired*
reads that the consistency loop compares, so the loop could still agree on two
corrupt readings. The invariant that matters is "no other CPU touches port 0x70
between selecting a register and reading it, for the entire duration of one
coherent time reading", and that is a property of `rtc_now`, not of `cmos_read`.

IRQ-saving because the same CPU must not re-enter through an interrupt handler
mid-sequence and leave the index pointing somewhere else.

The lock is held across the `rtc_updating()` spin, which can be up to ~2 ms
during an RTC update tick. That is accepted deliberately: `rtc_now` is a rare
call, and a bounded 2 ms hold is unambiguously preferable to a wall clock that
can run backwards.

## Gate

`make smoke-rtc-smp` — a ring-3 probe reads `SYS_CLOCK` in a tight loop under
`-smp 4` and asserts the reading is **monotonic** (allowing only a real midnight
wrap). Before the fix it observes a backwards jump within seconds; after it, it
does not.

This is a direct test of the invariant rather than a re-run of the symptom: the
metrics probe failing was three inferential steps away from the defect, which is
why it took so long to attribute.
