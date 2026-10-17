# DDR-1025 — `smoke-mouse` has been passing on a 1-in-5 margin

**Status:** MEASURED. Two instruments built, one hypothesis of mine **refuted**
by them. **No fix** — the root cause is not named, and the honest fix is a design
change that should not be made at speed.

---

## 1. The failure, and why a retry was not the answer

`smoke-mouse` failed on shard 2 on **two** documentation-only commits (`4fd2054`,
`43e853d`), both carrying kernel `53fe179c85a7c3b5`. A `workflow_dispatch` re-run
of `4fd2054` — the identical commit — **passed**. Pooled on that one binary:
**4 green, 1 failure across 5 completed suite-runs**, then a 6th run failed
again: **2 failures in 6.**

The first comment on this (PR #17, 5471708345) committed to *"an instrument on
the compositor's pointer-poll path, NOT a timeout bump"* if it recurred. It
recurred.

## 2. What the capture already ruled out

Both failing captures show `btnedge=5` in every heartbeat — the **driver** saw
five button press edges — while the compositor emitted **zero**
`PRADYOS_BTN_STATE` lines and the boot stayed healthy for the full 120 s (no
`[apfreeze]`, no `panics_silent`, no `panic_stage`).

Comparing event order against a local **passing** run settles what is missing:

| | local PASS | CI FAIL |
|---|---|---|
| `PRADYOS_SURFACE_OK 2` | line 431 | present |
| **`PRADYOS_BTN_STATE buttons=1`** | **line 433** | **absent** |
| `PRADYOS_CLOSE_OK id=2` | 436 | present |
| `PRADYOS_MOUSE_OK` | 437 | absent |
| `PRADYOS_SURFACE_GONE n=2` | 441 | present |

The compositor ran its **entire** sequence in CI and then emitted nothing but
heartbeats for another 5000 ticks. So it was alive; it just never observed a
button.

A **local** experiment shortening the injector's click hold from 200 ms to 4 ms
still **passed**, with `PRADYOS_BTN_STATE buttons=1 prev=0`. So "the press was
too brief for the poll interval" needs CI to be ~50× slower than local, which is
not a claim the evidence supports.

## 3. The instruments

Two counters, both surfaced in the existing heartbeat beside `btnedge`:

- **`mpoll` / `mbtn`** (`sys_input.c`) — how many times ring 3 asked for pointer
  state, and how many of those answers carried a button down. Counted at the
  syscall because the question is about what crosses the ring boundary.
- **`btn1drain`** (`virtio_input.c`) — drains of the input virtqueue that
  contained **both** a press and a release of the same button. DDR-941 records
  that `SYS_MOUSE_POLL` exposes current state rather than an event queue, so a
  press and release inside one drain leave *no instant* at which ring 3 could
  have looked. The 200 ms the injector waits between its two QMP commands is
  irrelevant if the guest drains both in one go.

## 4. Measured — on a PASSING run

```
btnedge=5 mpoll=205573 mbtn=1 btn1drain=0
```

**Two findings, and the second refutes me.**

**(a) The gate passes on one observation out of five.** Five press edges reached
the driver; across ~205,000 polls, **exactly one** ever returned a button down.
So the pointer path drops four of five injected clicks *even when the gate is
green*. A CI run where zero of five get through needs no new defect — it is the
same behaviour one draw further into the tail. **That alone explains a 2-in-6
failure rate**, and it means this gate has never had the margin its green
implied.

**(b) `btn1drain=0` refutes the IRQ-batch hypothesis** — mine, formed in §3.
Press and release are **never** coalesced into a single drain, so DDR-941's
by-construction case is not what is happening here. The instrument was built to
test that idea and it killed it, which is the same service DDR-1010's probe did
when it excluded its own SWAPGS path.

Also checked and excluded: `virtio_input_state()` does **not** consume on read
(`virtio_input.c`) — it returns `g_buttons` and leaves it set, so a held button
is not cleared by the act of polling.

## 5. ANSWERED — `mpollwin=0`. Ring 3 does not poll while the button is down.

Two further counters settle §5's question, and they contradict what §4 let me
infer.

- **`btnhold`** — the widest press→release span, in guest ticks.
- **`mpollwin`** — `sys_mouse_poll_count()` sampled at the press edge and again
  at the release: literally *"how many times did ring 3 ask while the button was
  down"*.

On a **FAILING** run (kernel `1d5122b64499d4d7`, reproduced locally):

```
btnedge=5 mpoll=96227 mbtn=0 btn1drain=0 btnhold=20 mpollwin=0
```

And the matching CI failure on `249da47` (kernel `2605f6d2b571e746`), the first
CI run carrying these counters:

```
btnedge=5 mpoll=159842 mbtn=0 btn1drain=0
```

**`btnhold=20` ticks is 200 ms** — the button is held for exactly the interval
the injector holds it, so the window is *not* sub-millisecond as §4 suggested.
**`mpoll` is ~96,000–160,000** — ring 3 polls roughly 750–1,400 times a second.
**`mpollwin=0`** — and **not one** of those polls falls inside the window, five
presses running.

Those three cannot all be true of a uniformly-polling client: ~150 polls should
land in each 200 ms press. **Ring 3 is not polling during the press window at
all**, however much it polls overall — the two are anti-correlated.

**`mpoll` being large also excludes the other family outright:** this is not
"the compositor stopped asking" or a liveness failure. It asks constantly, just
never then.

**Why it stops polling exactly then is a RING-3 question and is not answered
here.** The natural suspect is the compositor's own per-event work — the same
QMP sequence delivers absolute motion before the click, and a full composite
could span the window — but that is a hypothesis, not a measurement, and the two
hypotheses I have already formed in this DDR were both wrong.

**What IS established, and it is enough to justify the repair:** a state-only
`SYS_MOUSE_POLL` cannot deliver a click to a client that is not looking during
the press, *whatever* keeps it from looking. That is the API defect DDR-941
described as "invisible BY CONSTRUCTION", now measured rather than reasoned.

## 5b. What is still NOT established

**Why only one press in five is ever visible to a poll.** With ~1,800 polls/s and
a 200 ms hold, a press should span roughly 350 polls, and five presses ~1,750.
One was seen. Neither drain-coalescing, nor consume-on-read, nor poll starvation
of the compositor (it emitted its whole sequence) accounts for that gap.

§NON-NEGOTIABLE 3 therefore forbids a fix. Recording the measurement is the
finding.

## 6. The fix that is NOT being made, and why

The obvious repair is an **edge latch**: have the kernel remember that a press
occurred since the last poll, so a button pressed and released between two polls
is still delivered. That is a real product improvement — a desktop whose pointer
path drops 80% of clicks is a defect a user would hit, not just a flaky gate —
and DDR-941's "invisible BY CONSTRUCTION" note is really a statement that the
current API cannot express a click.

It is not being made here because it is a **kernel ABI semantic change** to
`SYS_MOUSE_POLL`, it would be built on a mechanism that §5 says is not yet
understood, and the release tag is currently held. Widening the gate's timeout or
its retry count is explicitly **not** the alternative: that would make the
symptom vanish while measuring strictly less, which is the failure mode DDR-1002
and DDR-1012 were each written about.

## 7. Measured

Kernel **`2605f6d2b571e746`**, `-Werror` clean, **1,134,986 B** — unchanged.
`smoke-mouse`, `smoke-input`, `smoke-compositor` all PASS, one hash verified
before and after each. `hygiene_check.sh` ALL THREE PASSED. The counters print
only inside the existing heartbeat line, so a healthy boot gains four short
fields and no new output.
