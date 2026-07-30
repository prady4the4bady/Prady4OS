# DDR-808 — OPEN-8: the fwcfg/`smoke-shell` mechanism, measured

**Status:** settling measurement RUN. **Stated mechanism refuted.** Causal claim
under re-test — no fix written.
**Date:** 2026-07-30
**Relates to:** DDR-804 (fw_cfg probe selection), DDR-789 (`$?` in PRISM),
DDR-803 (gate windows).

## What was claimed, and by whom

Carried into this session as established:

> `fwcfg_init()` does up to 4096 `inb` under TCG (each a VM exit), desynchronizing
> `smoke-shell`'s fixed-`sleep` timing against a FIFO.

**I wrote that figure, and it was never a measurement.** 4096 is the product of
the two S2 clamps in `fwcfg.c` — `FWCFG_MAX_ENTRIES (64) x 64-byte entries` — i.e.
the worst case the code permits, not the case it executes. It then propagated
through a handoff and a brief as though it were observed.

## The measurement

Instrumented `fwcfg_init()` to report the raw pre-clamp directory count and the
actual number of data-port reads, once per boot. Kernel `2f8aa0ecc7d4`,
`smoke-shell`'s own configuration:

```
[fwcfg] raw_dir_count=15 inb_count=968
```

The arithmetic confirms it exactly: 4 (signature) + 4 (count) + 15 x 64 (entries)
= **968**. The directory holds 15 entries against a clamp of 64, and the probes
file is absent on this boot so no payload read follows.

**968, not 4096 — a factor of ~4.2 out.** More importantly, 968 port reads are
microseconds of work. They cannot move boot timing by anything approaching the
0.5 s granularity of the gate's `sleep` intervals.

**Hypothesis 1 (fixed-sleep desync from VM-exit overhead) is refuted.**

## What this does *not* refute, and the honest problem with it

`fwcfg_init()` is still *correlated* with the failure: an earlier one-variable
arm with the call removed passed. But that arm was **a single run**, as was the
`90634b6` comparison. Against a gate that has now failed ~4 times and passed
twice, single-run arms do not establish causation — and `smoke-shell` is the most
timing-sensitive gate in the suite, so flakiness is exactly the alternative that
single runs cannot exclude.

Since the mechanism that justified the causal claim is gone, the causal claim
itself has to be re-earned. Re-running both arms at **3 runs each**, with kernel
SHAs printed so the arms are provably distinct:

* if WITHOUT-fwcfg passes 3/3 and WITH-fwcfg fails, `fwcfg_init()` is causal by
  some mechanism still to be found — and it is not overhead;
* if both arms fail intermittently, **`fwcfg_init()` was never the cause**,
  OPEN-8 is a flaky gate, and the fix is the gate's fixed-sleep driver
  regardless of fwcfg.

## The fix is not writable yet

The brief pre-authorises replacing the fixed sleeps with a sentinel-based wait.
That is very likely the right change *on its own merit* — a gate that proceeds on
observed readiness rather than on an arbitrary sleep is strictly better, and
DDR-803 already showed what unstated timing assumptions cost here.

But writing it now would mean shipping a fix whose stated justification has just
been measured false, against a causal claim resting on n=1 arms. If the gate is
simply flaky, the sentinel wait may not even address the `$?` assertion that
actually fails — that assertion is about **command ordering**, and a readiness
wait at the *start* does nothing for desync *mid-stream*.

So: finish the arms, then write §Fix against whatever they show.

## Note on the instrumentation

The `[fwcfg]` line stays. It costs one short print per boot, and it converts a
number that was asserted for two sessions into one that is observed on every
run — including in CI, where the directory count may differ from this
workstation's 15 and would then be visible rather than assumed.

## The causal claim is refuted too — `fwcfg_init()` was never the cause

Three runs per arm, distinct kernel SHAs so the arms are provably different
binaries:

| arm | kernel | result |
|---|---|---|
| WITH `fwcfg_init()` | `2f8aa0ecc7d4` | **FAIL 3/3** |
| WITHOUT `fwcfg_init()` | `ef7b998ae2eb` | **FAIL 3/3** |

Identical failure, identical assertion, both arms. `fwcfg_init()` is exonerated.

So OPEN-8's localization — carried across three sessions and into two briefs as
"established" — rested entirely on **one** passing run of the WITHOUT arm. That
single PASS is the only evidence that ever supported it, and 3/3 FAIL now
contradicts it.

### Which raises the sharper question

That configuration *did* pass earlier. It now fails 3/3. Something changed
between then and now, and the newest change is **mine**: the two `[boot-stamp]`
prints added in `ae2fdbf` — already pushed — sitting inside `fs_test_thread`
immediately before the probe block and before the SMP proofs.

If those prints are the cause, then instrumentation I added to make OPEN-1
self-reporting broke `smoke-shell`, and I published it. That has to be tested
before anything else, because every OPEN-8 measurement taken since `ae2fdbf`
would be contaminated by it.

Bisect running, 3 runs per arm:

* **NO-STAMPS** — only my two prints removed;
* **`90634b6`** — the last commit before DDR-804, green in CI.

Reading it:

* NO-STAMPS passes ⇒ my prints are the cause. Revert them, and OPEN-8 as
  originally described never existed.
* both fail ⇒ the prints are innocent, `90634b6`'s earlier single PASS was also
  luck, and `smoke-shell` has been failing deterministically on this workstation
  for longer than anyone noticed — in which case the CI/local split is the whole
  story and the gate's timing is the only thing to fix.

### Standing correction

Two claims about OPEN-8 are now withdrawn: the mechanism (VM-exit overhead,
refuted by `inb_count=968`) and the cause (`fwcfg_init()`, refuted by 3/3 FAIL on
both arms). What survives is only the observation that **`smoke-shell` fails on
this workstation and passes in CI**. Everything else was inference stacked on a
single run.

## THE ACTUAL MECHANISM — input character loss, not sleep desync

The bisect closed the question of *what changed*: nothing did.

| arm | kernel | result |
|---|---|---|
| NO-STAMPS (my `ae2fdbf` prints removed) | `0f4e11125970` | FAIL 3/3 |
| `90634b6` (pre-DDR-804, green in CI) | `8af2a20731fd` | FAIL 3/3 |

`smoke-shell` fails on this workstation at **every commit tested**, including one
CI proved green. My stamps are innocent, `fwcfg_init()` is innocent, DDR-804 is
innocent. Every earlier "it passes without X" was a single lucky run.

Then the serial showed what actually happens. Line 455:

```
prism> short-9x3
prism> prism: unknown command: erun          <-- HERE
prism> [sfs] lz4+tags compress/readback/tag OK
[sfs] persistent root provisioned; SFS-rooted probe spawned
st-fail=0
```

The feeder sends `echo st-ok=$?\n` and then `run /NOPE789.ELF\n`. PRISM received
**`e`** followed by `run /NOPE789.ELF` — concatenated into `erun`. Thirteen of
the fourteen characters of that command were **lost from the input stream**.

That cascades into exactly the two observed symptoms: `st-ok=` never appears
(the command never ran), and `st-fail=0` instead of `127` (the failing `run`
never executed, so the status it reports is meaningless).

**This is not a timing desync, and the gate's fixed sleeps are not the cause.**
The stream completed — the log ends with `init: reaped PID=42 exit=0`, 25 KB, 37
prompts, all later commands present. Nothing was cut off. One command was
*corrupted in transit*.

### Leading mechanism, with the evidence for it

`kputs`/`kwrite` wrap their whole loop in `irq_save()` … `irq_restore()`, and
each character inside spins on UART THRE (the unbounded wait of DDR-807). While
that runs, **IRQ4 cannot fire**, so the handler that drains COM1's 16-byte RX
FIFO into the 256-byte ring cannot run either. A burst of kernel output therefore
holds input off long enough for the FIFO to overflow, and overflowed bytes are
gone.

The position of the single loss supports this: it lands between
`[sfs] lz4+tags …` and `[sfs] persistent root provisioned …` — i.e. exactly
during a burst of concurrent kernel output, at the moment the feeder was writing.

It also explains the CI/local split without appealing to "slow runners": on a
host that drains COM1 quickly the THRE spin is short and the IRQ-off window
never grows enough to overflow 16 bytes; on this workstation it does.

### Consequences, if it holds

* **The fix is NOT in the gate.** The brief's directive — replace fixed sleeps
  with sentinel waits — addresses a mechanism that has now been refuted twice
  over. A sentinel wait cannot recover a character the UART already dropped. It
  is still a worthwhile robustness change on its own merit, but it would not fix
  this, and shipping it as the fix would leave a real kernel defect in place
  wearing a green gate.
* **This is a kernel input-integrity defect**, and it is coupled to DDR-807: the
  unbounded THRE spin is what makes the IRQ-off window long enough to matter.
  Bounding that wait shortens the window, which is a second, independent reason
  to do it.
* Any PRADYOS console consumer — not just this gate — can silently lose typed
  input during heavy kernel output. That is a correctness problem for the OS,
  not a test-harness inconvenience.

### Not yet proven

One loss event, in one log. Running 3 more to see whether the loss point varies
(transient overflow) or repeats at the same command (something deterministic).
Given this investigation's record, that distinction gets measured before any fix
is written.

## CONFIRMED — transient RX FIFO overflow during IRQ-off output bursts

Four runs, four losses, kernel `4923c1831f2a`:

| run | mangled command | st-ok=0 present | st-fail line |
|---|---|---|---|
| earlier | `erun` | no | `st-fail=0` |
| 1 | `ecat` | no | absent |
| 2 | `eecho` | no | absent |
| 3 | `eecho` | no | absent |

Exactly **one** loss per run, and the loss point **varies** — `run`, `cat`,
`echo` are different points in the feeder stream. So nothing deterministic is
eating a particular command; the loss lands wherever an output burst happens to
coincide with the feeder writing.

The signature is identical every time: the leading `e` of an `echo` survives,
the remainder of that command is lost, and the next command concatenates onto
the orphan `e`. That is precisely a FIFO with one slot free at the moment the
burst begins — one character accepted, the rest dropped while IRQ4 is held off,
and normal service resumed by the time the next command arrives 0.5 s later.

**OPEN-8 is a kernel input-integrity defect.** It is not gate timing, not
`fwcfg_init()`, not DDR-804, not my instrumentation. All four were refuted by
measurement.

## Fix direction — needs its own DDR, three options with real trade-offs

The window exists because `kputs`/`kwrite` hold `irq_save()` across the whole
loop while each character spins on THRE. Candidates:

1. **Bound the THRE spin (DDR-807).** Shortens the window; does not close it.
   Worth doing anyway, but not a fix for this on its own.
2. **Re-enable interrupts between characters.** Closes the window, but destroys
   the atomicity `kwrite` exists to provide — ADR-030 requires a whole buffer to
   emit without another CPU's output interleaving. Rejected on those grounds
   unless that guarantee is renegotiated.
3. **Drain RX inside the TX spin.** While waiting for THRE, also test LSR
   data-ready and push any arriving byte into the RX ring. Keeps interrupts off,
   keeps atomicity, and removes the loss at its source.

Option 3 is the most promising and also the most delicate: `console.c` documents
the RX ring as **single-producer** (the IRQ) / single-consumer (`sys_read`), and
this would add a second producer on the output path. That invariant is what makes
the ring lock-free today, so the fix must either prove the two producers cannot
race (they can — IRQ4 on another CPU under `-smp 4`) or change the discipline.
That is a real design question, not an edit.

No code in this slice. The mechanism is established; the fix is not designed.
