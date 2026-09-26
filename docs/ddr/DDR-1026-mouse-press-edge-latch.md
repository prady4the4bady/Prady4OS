# DDR-1026 — A press-edge latch for `SYS_MOUSE_POLL`

Status: **IMPLEMENTED + GATED + mutation-checked (M1)**
Builds on DDR-941 and DDR-1025. **Corrects DDR-1025 §5 on its central reading.**

---

## 1. DDR-1025 §5 read `mpollwin=0` wrong, and this is the correction

DDR-1025 §5 ended on this counter set, from a failing `smoke-mouse`:

```
btnedge=5 mpoll=96227 mbtn=0 btn1drain=0 btnhold=20 mpollwin=0
```

and drew from it: ring 3 polls ~1,000 times a second yet **zero** of those polls
fall inside any of the five 200 ms press windows, so its poll cadence must be
anti-correlating with the injector for reasons unknown. That framing made the
defect look deep and unexplained. It was neither.

`mpoll` is **cumulative over the whole boot**, and the failure is at the start of
it. The first run of the latch kernel printed the heartbeat sequence that
settles it:

```
btnedge=0 mpoll=0 mbtn=0     <- ... eleven heartbeats like this ...
btnedge=3 mpoll=0 mbtn=0     <- THREE presses have landed. Ring 3 has polled 0 times.
btnedge=5 mpoll=1 mbtn=1     <- the first poll of the entire boot, after all five
btnedge=5 mpoll=3481 mbtn=1
btnedge=5 mpoll=13399 mbtn=1
```

Ring 3 was not missing each window. **It had not polled once.** All five clicks
are injected before the compositor's input loop takes its first sample, and the
~1,000 polls/s begins afterwards — which is exactly why a counter summed over
80 s said nothing about the six seconds that mattered. `mpollwin=0` is a
tautology in that regime, not a finding.

`smoke-mouse` fires its five clicks on the readiness sentinel
`PRADYOS_AMBIANCE_OK`. That sentinel does not mean the compositor is servicing
input; it means the ambiance render finished. `mouse_inject.sh` has carried an
outcome-driven retry since DDR-910, whose own header comment names this exact
situation — *"If the compositor had not yet been scheduled to service input, all
five landed on the floor and no further click was ever sent"* — and `smoke-mouse`
is the gate that never adopted it, passing `$4` empty and taking the bounded
five-click path.

## 2. Why the fix is still the latch, and not the injector argument

Adding `PRADYOS_MOUSE_OK` as the injector's `$4` would turn the gate green by
clicking until one landed. That is the timeout/retry bump DDR-1025 ruled out,
and it would leave the actual behaviour untouched: **the five clicks a real user
made during that window would still be gone.**

The reason they are gone is the ABI. `SYS_MOUSE_POLL` exposes *current state*,
not an event queue (DDR-941), and a state poll cannot represent an edge that has
already ended. Nothing on either side of the boundary can fix that: the driver
saw all five presses (`btnedge=5`), and ring 3 asked correctly — it just asked
after the button came back up.

So the defect is in the contract, and it is a genuine product defect, not a gate
artefact: a desktop that silently drops every click made before its compositor's
first input sample — and any click that completes between two samples — is
user-visible.

## 3. The change

`kernel/drivers/input/virtio_input.c` gains one word of state:

```c
static volatile uint32_t g_btn_latch;   /* press edges not yet reported to ring 3 */
```

set in `fold_event` on the same edge that already increments `g_btn_edges`, and
drained by a read-and-clear accessor:

```c
uint32_t virtio_input_btn_latch(void) {
    return __atomic_exchange_n(&g_btn_latch, 0, __ATOMIC_SEQ_CST);
}
```

`sys_mouse_poll` ORs it into the reported mask:

```c
btn |= virtio_input_btn_latch();
```

Read-and-clear on a driver accessor is the established pattern here, not a new
one: `virtio_input_wheel()` (DDR-725) has worked exactly this way since Layer 7,
for the same reason — detents accumulate between polls and would otherwise be
lost.

`virtio_input_state()` is deliberately left **pure**. It is the driver's
read-only view; making it mutate on read would be a trap for the next reader.
The latch is drained at the syscall, which is where the ring boundary — and
therefore the "has this been delivered yet?" question — actually is. There is
exactly one caller of either, so this costs nothing in duplication.

### 3.1 Resulting semantics

| sequence | before | after |
|---|---|---|
| press, poll (still down), release, poll | down then up ✅ | unchanged ✅ |
| press, release, poll, poll | nothing — click lost ❌ | down then up ✅ |
| press, release, press, release, poll | nothing ❌ | **one** click, not two ⚠ |
| press, poll, release, press, poll | down, then still down (release lost) ❌ | unchanged ❌ |

Rows 3 and 4 are the bounded residual, and they are the reason the gate asserts
`>= 1` rather than `>= 5` (§4). The latch is a **bitmask, not a counter**: it
records *that* a press is pending, not how many. Row 4 needs a release latch,
which would require emitting `0` and then `1` from two consecutive polls — an
event queue, a far larger ABI change than this one. Not built.

Making the latch a counter would fix row 3 but not row 4, and would hand ring 3
a `buttons` mask whose bits no longer mean "down" — a worse contract than the
one being repaired. Not built.

### 3.2 Position fidelity

The latched press is reported at the **current** pointer position, not the
position at the press edge. For the case the latch exists to fix — a click in
place — those are the same point, and the gate confirms it (`PRADYOS_MOUSE_OK
500 281`, the injected coordinate, on every run). A press, release **and** move
between two polls would land the synthetic click at the moved position; today
that case is invisible entirely, so the latch is strictly better there, and
carrying a latched coordinate would make the compositor draw the cursor at a
stale point for one frame. Not built. Recorded.

## 4. The gate arms, and the ordering that keeps both live

The old gate asserted `PRADYOS_MOUSE_OK` **at least once** out of five injected
clicks, and nothing kernel-side. That is why it passed on a 1-in-5 margin for
months (DDR-1025 §3): one surviving click satisfied it, and a run where none
survived needed no new defect to explain.

Two arms now, one per side of the ring boundary:

- kernel: `mbtn >= 1` from the last heartbeat — `SYS_MOUSE_POLL` returned a
  button-down to ring 3 at least once.
- ring 3: `PRADYOS_MOUSE_OK` present — the compositor acted on a down-edge.

**The kernel arm runs first, and that ordering is load-bearing.** `mouse_ok >= 1`
implies `mbtn >= 1`, so with the ring-3 arm first the kernel arm can never be the
thing that fires — decoration, not measurement, the failure this project has now
hit six times (DDR-1016 §5, 1017 §4, 1018 §3, 1020 §5 twice, 1023 §5). This was
not reasoned about, it was **measured**: the first M1 run tripped the
`PRADYOS_MOUSE_OK` arm and never reached the new one. Checked kernel-side first,
the pair splits the failure cleanly — `mbtn=0` means the syscall never delivered
the press; `mbtn>=1` with no `PRADYOS_MOUSE_OK` means it delivered and ring 3
did not act.

The thresholds are `1`, not `5`, and that is a measurement rather than slack:
§1 shows all five edges land before ring 3's first poll, and §3.1 row 3 shows the
bitmask collapses them into one deliverable click. Demanding more would assert
something the injection timing cannot supply.

The injector is **not** changed, and that is deliberate: leaving it on the
bounded five-click path keeps the dead-window case — the one the latch exists
for — inside the gate. Adopting DDR-910's retry here would remove the only
coverage this change has.

## 5. Measurements

All on `smoke-mouse`, kernel hash recorded per §R1.

| build | kernel | result | counters |
|---|---|---|---|
| fixed | `56a4c4a35c92cfc5` | **PASS ×4/4** | `mbtn=1 mouse_ok=1`, identical every run |
| M1 mutant | `698ac2d1ceaad30d` | **FAIL** | `mbtn=0 mouse_ok=0` |

**M1** replaces the delivery with `(void)virtio_input_btn_latch();` — the latch
is still set and still drained, only the OR into the reported mask is cut. It
fails at the kernel-side arm, deterministically, on the arm that names the
latch.

4/4 identical is itself the result worth stating: the gate used to fail roughly
2 runs in 6 (DDR-1025 §2) with `mbtn` reading 0 or 1 unpredictably. It is now
deterministic, because the latch cannot be lost to timing — it persists until a
poll consumes it.

## 6. What this does NOT do

- It does not explain why the compositor's input loop starts as late as it does,
  and does not make the injector's readiness sentinel correct. `PRADYOS_AMBIANCE_OK`
  still does not mean "servicing input". The latch makes that *not matter* for
  click delivery; it does not make it right.
- It does not fix rows 3 and 4 of §3.1 — repeated clicks between two polls still
  coalesce, and a missed release is still missed.
- It is not a fix for anything in OPEN-1, OPEN-2, OPEN-12 or OPEN-13.
