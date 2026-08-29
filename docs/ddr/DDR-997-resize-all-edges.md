# DDR-997 — resize from any edge, not just the bottom-right corner

**Status:** IMPLEMENTED, GATED, MUTATION-CHECKED (M1/M2/M3 all caught).
**Extends:** DDR-718 (bottom-right 14x14 resize corner), DDR-894 (`rz=` geometry).
**Gate:** `smoke-resizeall` (new), alongside the existing `smoke-evresize`.

---

## 1. What exists, and the one thing that makes this non-trivial

`user/compositor.c:1260` hit-tests a single 14x14 square at the bottom-right of
each window. On release (`:1293`):

```c
int neww = ms.x - rs_bx, newh = ms.y - rs_by;   /* rs_bx/by = the surface ORIGIN */
```

That works because the bottom-right drag **leaves the origin fixed**: only width
and height change, and one `SYS_SURFACE_SENDEV(type 1)` carries both.

A north or west drag does not have that property. Pulling the left edge leftwards
must *both* widen the window *and* move its origin left. The compositor has two
separate calls for those — `SYS_SURFACE_MOVE` and the type-1 resize event — so
the operation is **not atomic**, and the order is a real decision rather than a
detail (§3).

This is the whole reason the item was left as "bottom-right only" and is worth a
DDR rather than a patch.

## 2. Handles

Eight regions per window, 14 px thick (the DDR-718 corner size, kept so the hit
target does not change size between old and new handles):

```
NW  N  NE      corners: 14x14 squares
 W  .  E       edges:   14 px deep strips between the corners
SW  S  SE
```

SE keeps its existing behaviour bit-for-bit — it is the one path with a green
gate today (`smoke-evresize`), and it must not regress.

The title bar already owns move-drag (DDR-705), so the N strip is taken from the
window's top edge ABOVE the title bar is NOT available. **N and NW/NE are
therefore hit-tested on the frame edge only, and the title-bar test runs FIRST**
— an ambiguous pixel belongs to move, not resize, because move is the older and
more frequently used gesture.

## 3. The origin-shift decision: MOVE first, then resize

For a W or N drag both a move and a resize are required. Two orders are possible
and they are not equivalent:

- **resize then move** — the window first grows/shrinks about its *old* origin,
  then jumps. For a leftwards W drag it visibly grows to the RIGHT and then
  snaps left: a one-frame artifact in the wrong direction.
- **move then resize** (CHOSEN) — the origin lands first, then the size follows.
  The intermediate frame is the window at its new position with its old size,
  which is the same shape a plain move already produces, so it reads as a move
  that then settles.

Neither is atomic and this DDR does not pretend otherwise. The choice is which
intermediate frame is less wrong, and "already looks like an existing gesture"
wins over "moves opposite to the pointer".

The client redraws asynchronously either way (DDR-718's `recompose_scene()`
comment already says so), so the intermediate is bounded by the client's
response, not by the compositor.

## 4. Clamps, and a trap

`neww`/`newh` clamp to [32, 512] exactly as DDR-718 does — 512 is
`SURFACE_DIM_MAX` (`sys_surface.c:17`), not a compositor preference.

**The trap:** on a W or N drag the clamp must be applied BEFORE deriving the new
origin, not after. Clamping the size afterwards leaves the origin where the
unclamped drag put it, so a window dragged past the 32 px floor keeps sliding
while its width stays pinned — the edge separates from the pointer. Derive the
clamped size first, then place the origin from the FIXED edge:

```
W drag:  neww = clamp(x0 + w0 - mx);  newx = (x0 + w0) - neww;
N drag:  newh = clamp(y0 + h0 - my);  newy = (y0 + h0) - newh;
```

The fixed edge (right for W, bottom for N) is the invariant, and both the size
and the origin are derived from it. That makes the clamp self-consistent.

## 5. Geometry must be published, not hardcoded (§INV.5 / §NON-NEGOTIABLE 9)

`PRADYOS_WM_GEOM` already carries `rz=X,Y` for the SE corner (DDR-894). Gates
must never hardcode pixel coordinates, so the new handles are published the same
way — one field per handle, each the CENTRE of its hit region:

```
rzn=X,Y rzs=X,Y rzw=X,Y rze=X,Y rznw=X,Y rzne=X,Y rzsw=X,Y
```

`rz=` is left alone, meaning SE, so every existing parser keeps working. §INV.5's
warning applies: a parser must isolate each field before splitting on `,`.

## 6. Gate — `smoke-resizeall`

For each of the four edges, drive a drag from the published handle centre and
assert the committed geometry:

- **E / S** — origin unchanged, one dimension changed. The cheap arms.
- **W** — `PRADYOS_RESIZE_REQ` width changed AND a `PRADYOS_DRAG`/move to the new
  origin, with `x_new + w_new == x_old + w_old` (the right edge held still).
  That equality is the assertion; a width-only check passes on a broken origin.
- **N** — the same with `y_new + h_new == y_old + h_old`.

The invariant-based arms (W, N) are the load-bearing ones: they encode §4's fixed
edge, so a mutant that resizes without moving, or moves without resizing, fails
them. A test that only asserted "width changed" would pass both mutants.

## 7. Mutation checks (required)

- **M1** — drop the move on a W drag. The W arm must fail on the fixed-edge
  equality while E/S still pass.
- **M2** — apply the clamp after deriving the origin (§4's trap). Drag past the
  32 px floor; the fixed-edge equality must break.
- **M3** — hit-test resize before the title bar. Move-drag on the title bar must
  break, catching the §2 ordering.

## 8. NOT in scope

- No aspect-ratio locking, no snapping, no keyboard resize.
- No minimum-size negotiation with the client: 32 px is imposed, as DDR-718 does.
- The 512 ceiling stays. Lifting it is `SURFACE_DIM_MAX` and a PMM budget change
  across many gates — sized separately, deliberately not bundled here.


---

## 9. Implementation — what was measured

Kernel hash **`6f0da11f2ef4a123`**, `kernel.bin` 1,085,834 B against the
1,572,864 B gate. `make image` warning-clean at `-Werror`.

`smoke-resizeall` drives four drags in ONE boot and passes:

```
arm e OK — (140,140 64x64)   -> (140,140 157x64),  1 observation(s)
arm s OK — (140,140 157x64)  -> (140,140 157x117), 1 observation(s)
arm w OK — (140,140 157x117) -> (265,140 32x117),  1 observation(s)
arm n OK — (265,140 32x117)  -> (265,225 32x32),   1 observation(s)
```

The two load-bearing equalities hold exactly: `140+157 = 297 = 265+32` (W) and
`140+117 = 257 = 225+32` (N). Both shrink arms reached the 32 px floor, so the
clamp is genuinely exercised rather than merely present.

### 9.1 The FIX line had to report the OBSERVED origin, not the intended one

The first version emitted `x=`/`y=` from the compositor's own `newx`/`newy`.
Under M1 — drop the `SYS_SURFACE_MOVE` and change nothing else — `newx` is still
computed and would still have been printed, so **the gate would have passed a
window that never moved**. That is the identical decorative-arm mistake DDR-996's
first arm B made, caught here before it was believed rather than after.

The fix is a re-poll: `SYS_SURFACE_POLL` after the move, reporting the surface's
actual `x`/`y`. `w`/`h` stay the REQUESTED values, because the client honours the
resize asynchronously (that round-trip is `smoke-evresize`'s job). Mixing an
observed origin with a requested size is deliberate and is exactly the property
under test — the origin actually moved to must complement the width actually
asked for.

### 9.2 Mutation results — three mutants, three distinct kernel hashes

| Mutant | Kernel hash | Result |
|---|---|---|
| (none) | `6f0da11f2ef4a123` | `smoke-resizeall` PASS, `smoke-drag` PASS |
| **M1** — drop the move on a W/N drag | `34ef019aa3fdccd5` | W and N FAIL, **E and S still pass** |
| **M2** — clamp after deriving the origin | `c683670acf34792a` | W and N FAIL |
| **M3** — resize hit-test before the title bar | `018e1777db0547fb` | **`smoke-drag` FAILs** |

M1 and M2 fail with **different** signatures, so the gate discriminates them
rather than merely reporting "something is wrong":

- M1: `x+w=172, was x0+w0=297` — the origin never moved, so the right edge
  travelled LEFT by the amount the width shrank. The dedicated
  `origin did NOT move` check also fires.
- M2: `x+w=322, was x0+w0=297` — the origin moved to the UNCLAMPED pointer
  position and then the width was floored under it, so the right edge overshot
  RIGHT by exactly `32 - 7 = 25`. The `origin did NOT move` check correctly does
  NOT fire here: the origin did move, just to the wrong place.

E and S surviving M1 is the point §7 makes: an arm that only asserted "the width
changed" would have passed both mutants.

### 9.3 M3 is NOT vacuous, and the reason is worth recording

§2 says the title bar must be hit-tested first. On a single surface that
requirement is now structural rather than ordered: the title bar occupies
`y-TITLEBAR .. y` and the N band occupies `y .. y+RZBAND`, so the two regions are
**disjoint** and no ordering can change the outcome. Reading only that, M3 looks
untestable.

The ambiguity is **cross-surface**, and it is real in the shipped layout. ALPHA
sits at (100,100) 64x64, so its east band is `x >= 150` over `y 100..164`. BETA
sits at (140,140), so its published title-bar drag point `dg=` is (150,131) —
**inside ALPHA's east band**. Under M3 the press therefore grabs ALPHA's east
edge instead of moving BETA, and `smoke-drag` fails with `drag did not start on
the title bar` while the log carries the giveaway:

```
PRADYOS_RESIZE_FIX id=0 edge=8 x0=100 y0=100 w0=64 h0=64 x=100 y=100 w=300 h=64
```

id=0 is ALPHA, `edge=8` is `RZ_E`. Predicted from the published geometry before
the run, then confirmed by it.

### 9.4 One bug found in the gate itself, by the serial log

The first `smoke-resizeall` run had E, W and N green and **S failing on every
attempt** — a suspicious pattern, since S is the easiest arm (origin fixed, one
dimension). The compositor's own `PRADYOS_BTN_STATE` lines (DDR-941) settled it
without touching the resize code: the compositor observed **6 of the 10 injected
button edges**, and the missing pair was S's.

Cause was in the injector, not the kernel. It waited for "any new
`PRADYOS_WM_GEOM` line" between arms; in the capture that wait was satisfied by
an unrelated republish (GAMMA closing) **before the E arm's resize had even
committed**, so the S drag was injected while the compositor was still inside E's
client round-trip and recompose. `SYS_MOUSE_POLL` reads current state rather than
an event queue (DDR-941), so a press and a release that both fall inside one busy
window are not queued — they are simply never seen. The wait now requires a geom
line published *after* this arm's own drags.

This is §INV.8's lesson in a different costume: the failure was a claim about
timing, and reading the timing instrument first was cheaper than reading the code.

### 9.5 Geometry republish was stale, repo-wide

`PRADYOS_WM_GEOM` was emitted only when the surface COUNT or the focus changed,
so after any move or resize the last published line described a window that had
since moved. `smoke-evresize` never noticed because it does one drag and stops.
Four drags in a row do notice. The publish condition now also fires on a rect
change, tracked exactly (per-slot `x/y/w/h`), not hashed — a hash collision here
would silently republish nothing.

### 9.6 Regressions checked

`smoke-evresize`, `smoke-drag`, `smoke-wmclose`, `smoke-wmmax`, `smoke-wmmin`,
`smoke-mouse`, `smoke-agent-click`, `smoke-shell` (5/5), `smoke-blkmq`,
`smoke-rqstress-liveness`, `smoke-blk-integrity` — all PASS on
`6f0da11f2ef4a123`. `ci-shard-check` OK at **155 gates / 10 shards / 7 excluded**;
`ci-probe-rodata-check` OK.

### 9.7 What this does NOT claim

The four arms exercise one surface at 1024x768 with a client that honours resize
requests. Not covered: a client that ignores or partially honours a request, a
window dragged off-screen (`SYS_SURFACE_MOVE` clamping is untested here), and
simultaneous drags on two surfaces. The 512 ceiling stays untouched per §8.


---

## 10. §9.8 — the gate's first CI run was RED, and the OS was not at fault

`cbc8a88` failed shard 9 twice. Arm E passed; S, W and N failed. The measured
cause is in the injector, and the fixed-edge assertions had already held:

```
arm w: line: edge=4 x0=140 y0=140 w0=157 h0=57 x=147 y=140 w=150 h=57
```

`147 + 150 = 297 = 140 + 157`. The invariant §6 calls load-bearing was satisfied
on the failing run. What failed were the auxiliary clamp checks — `w` was 150,
not the 32 px floor — because **every arm committed at its own drag START
coordinate**: `neww = (x0+w0) - ms.x` with `ms.x = 147` is exactly the press
point. Arm E was the exception because it happened to be observed at its end.

The injector released after a fixed 0.45 s, betting the compositor had polled in
between. `SYS_MOUSE_POLL` reads current state rather than an event queue
(DDR-941), so a pointer move falling entirely between two polls is never
observed at all — the bet holds on a quiet local machine and loses on a loaded
CI runner. This is the *same* defect class as §9.4, one phase later: there the
press was missed, here the drag was.

Fix: `PRADYOS_RESIZE_TRACK id=N x=X y=Y`, one line per drag, emitted the first
time the compositor sees the pointer away from the press point. The injector
waits for it before releasing. A duration becomes a precondition — DDR-910's
rule ("poll for the outcome, never a fixed wait") applied to the drag phase
instead of the click.

Two things worth keeping:

1. **The clamp checks earned their place.** Without them the run would have
   passed on the fixed-edge equality alone while silently no longer exercising
   M2. The check that fired said so in its own failure text.
2. **The local run now takes CI's path.** After the fix the S arm resolves
   `6982,8416` — the post-E geometry, which is what CI resolved and the earlier
   local runs did not. The reproduction is no longer weaker than the environment
   it is defending against.

---

## 11. §11 — CI red again, and this time the gate's *generator* was wrong

`smoke-resizeall` failed shard 9 on `0c22334`. Three arms red, one green. The
injector's own output named the trigger before any code reading was needed:

```
[resize_inject] no RESIZE_TRACK within 6s — compositor never observed the drag; retrying this round
```

— once for E, twice for S, twice for W, once for N.

### 11.1 What actually went wrong (two defects, both mine)

**(a) Geometry was resolved once per ARM and reused by every round.** When a
round partly landed, the window moved; the next round then dragged from
coordinates that had been correct *before* that move, hitting a different band.
That is where this line came from:

```
[resizeall] FAIL — arm e: E drag did not widen: w0=150 w=143
            line: edge=8 x0=147 y0=140 w0=150 h0=57 x=147 y=140 w=143 h=57
```

`x0=147, w0=150` is the geometry *after the W arm had already moved and shrunk
the window* — so this is an E-arm **retry**, logged as a shrink. The arm's own
first observation was correct and is in the same log:
`edge=8 x0=140 y0=140 w0=64 h0=64 → w=157`.

**(b) The 6 s `RESIZE_TRACK` window is too short on a loaded runner.** §10 added
that wait precisely so a release could not happen before the compositor had
observed the drag — but on timeout it warned and released anyway, which is the
blind release it exists to prevent. Locally 6 s was ample; in CI it never once
sufficed.

### 11.2 The fix, and the one I deliberately did NOT make

Re-resolve the handles **per round**, and widen the TRACK window to 20 s.

The tempting alternative was to loosen the checker — score only each arm's
*first* observation and ignore retries. That would have turned this log green
without changing anything real, and it would have discarded the rule that makes
the checker worth having ("every observation must hold; one good drag does not
excuse a bad one"). That rule is **correct for independent repeats and wrong
only for retries against stale geometry**, so the honest repair is to stop
generating stale-geometry retries. Fixing the generator keeps the assertion at
full strength; loosening the checker would have hidden the defect it caught.

### 11.3 What this says about §10

§10's conclusion — "a duration becomes a precondition" — was right in direction
and under-sized in magnitude. The precondition was real; the timeout on it was
still a duration, and I picked it from local timing. The measured lesson is
narrower than "poll, don't sleep": **a fallback that fires on timeout inherits
every problem of the sleep it replaced**, unless the fallback itself is safe.
Here it was not: it released blind.

### 11.4 NOT yet validated

The fix is injector-side only (`tools/qemu_runner/resize_inject.sh`), so it does
not touch the kernel — deliberately, because the OPEN-1 E1 campaign is mid-flight
and owns the only QEMU (§NON-NEGOTIABLE 12). The embedded Python parses and the
shell lints, but **`smoke-resizeall` has not been run against it**. It is not
claimed fixed until it has.

---

## 12. §12 — §11's diagnosis was right about the symptom and WRONG about the cause

§11 blamed stale handles and fixed them by re-resolving per round. That change
was correct but not sufficient, and the reason is worse than the bug it patched:
**the retries should never have happened at all.**

### 12.1 The measurement that settled it

After §11, the gate began failing **locally** — which is progress, because it made
the failure debuggable off CI. The log said everything:

```
PRADYOS_RESIZE_TRACK id=1 x=297 y=172        <- the compositor DID see the drag
PRADYOS_RESIZE_FIX  id=1 edge=8 x0=140 w0=64 -> w=157   <- round 0 COMMITTED correctly
[resize_inject] arm=e round 1 re-resolved start=6309,7348   <- ...and it retried anyway
```

Round 0 of arm E worked. The injector retried regardless, because the retry
decision —

```python
drag(sx, sy, ex, ey)
if fix_count(bit) > before_fix:   # checked IMMEDIATELY
    break
```

— was evaluated the instant `drag()` returned. The compositor commits on the
release and then has to get the line out; checking at once scores a **successful**
round as failed. The retry then dragged against geometry that had not been
republished yet, producing exactly the wrong-arm observations §11 attributed to
stale handles.

The handles were stale **because the retry happened**, not the other way round.

### 12.2 The fix

Poll for the commit (bounded, `FIX_WAIT_S = 8 s`) before deciding to retry; and
when a retry *is* justified, wait for a `PRADYOS_WM_GEOM` line published **after**
that round before re-resolving. §9.4 already established that rule — it was
applied between ARMS and never between ROUNDS.

### 12.3 The pattern, now three deep

This is the **third** appearance of one mistake in this file, and naming it is
worth more than any of the individual fixes:

| § | what was checked too early |
|---|---|
| §9.4 | the **press** — released before the compositor had polled |
| §10 | the **drag** — released before the compositor had seen the move |
| §12 | the **commit** — retried before the compositor had logged it |

`SYS_MOUSE_POLL` reads current state, not an event queue (DDR-941), and the
serial log is written asynchronously. Every interaction with this compositor has
a *report latency*, and every place the gate tests a condition it must first wait
for the report. Three separate bugs, one root: **asking before the answer exists.**

### 12.4 Measured

Kernel `5349db4d791cc2ab`, three consecutive local runs:

```
run 1: PASS — 0 retries
run 2: PASS — 0 retries
run 3: PASS — 0 retries
```

**Zero retries** is the load-bearing number, not the PASS. Before this fix even
the passing runs churned through retries — the buggy path was being entered every
time and merely getting away with it. Now round 0 succeeds and is recognised as
succeeding, so the retry path is not entered at all locally. Under CI load
retries will still occur, and they will now be correct.

### 12.5 Also fixed: a message that lied

The timeout was raised to 20 s in §11 but its message still printed
"no RESIZE_TRACK within 6s". A diagnostic that misreports its own threshold is
how an investigation gets sent to the wrong place — it is now derived from the
constant.

---

## 13. §12's fix made the FAILING path worse, and CI said so — `a74e086` is incomplete

§12 raised `TRACK_WAIT_S` from 6 s to 20 s and added an 8 s `FIX_WAIT_S`, so a
retry could no longer be triggered before the commit had reached the log. That
reasoning still holds. **The arithmetic around it did not**, and CI run
33247210328 (shard 9, `6b3ffbb`) is the artefact — a commit that PREDATES
`a74e086`, so it shows the old timings, but it shows the shape of the problem
with the new ones written into it.

### 13.1 The budget does not fit

The gate caps the guest at `timeout 180` (`Makefile:3029`). Measured from that
job's own timestamps, boot to first injection is **49 s** (11:21:30 → 11:22:19),
leaving ~131 s for four arms.

| | per round | 4 arms × 3 rounds, worst case |
|---|---|---|
| before §12 | 6 s | 72 s — fits |
| after §12 | 20 + 8 = 28 s | **336 s — does not fit** |

So a run that needs to retry is now guaranteed to be SIGTERM'd rather than to
report. That is not a hypothetical: the `6b3ffbb` job died exactly that way —

```
qemu-system-x86_64: terminating on signal 15 from pid 13642 (timeout)
[resizeall] FAIL — arm w: no PRADYOS_RESIZE_FIX line with edge&4 for id=1
[resizeall] FAIL — arm n: no PRADYOS_RESIZE_FIX line with edge&1 for id=1
```

Arm w was mid-drag when the guest was killed and **arm n never ran at all**. Its
"FAIL" is therefore not a measurement of arm n; it is the absence of one, printed
in the same words a real failure would use. A gate that cannot distinguish "this
arm is broken" from "this arm never executed" is the vacuity trap DDR-973 §6 and
DDR-996 each caught once, in a new costume.

### 13.2 The waits are upper bounds, which is why this survived review

On a healthy run `RESIZE_TRACK` arrives in well under a second and both loops
break early, so the happy path costs almost nothing and the change looks free.
The cost appears only once something is already wrong — precisely the run whose
diagnosis matters most. "It is fast when it passes" is not a defence of a
timeout budget.

### 13.3 The dropped press, which is a separate and still-open question

One thing in that log is NOT explained by retry timing, and is recorded here so
it is not lost. Arm w pressed at QMP `4708,8458` and released at `9288,8458`.
At 1024×768 those map to guest **(147,198)** and **(290,198)** — the scale is
confirmed by the gate's own `PRADYOS_MOUSE_OK 290 198` and, for arm s,
`PRADYOS_MOUSE_OK 172 257` against `end=5509,10976`.

BETA was then `x0=140 w0=157`, spanning x 140…297, so with `RZBAND 14` the west
band is 140…154 and the east band is 283…297. The press at 147 is **west**. The
committed line was

```
PRADYOS_RESIZE_FIX id=1 edge=8 x0=140 y0=140 w0=157 h0=117 x=140 y=140 w=150 h=117
```

`edge=8` is `RZ_E`, and `neww = ms.x - x0 = 290 - 140 = 150` reproduces the
published `w=150` exactly. So the compositor did not latch the west press at
all: it saw the **release** at 290, in the east band, and treated it as a fresh
east-edge press. A lost press, not a misclassified one.

Whether §12's fix also cures that is **unknown and must not be assumed** — §12
changed when the harness retries, not whether the guest observes a press. The
first post-`a74e086` CI result should be read for arms w and n specifically, and
this section is the prediction to check it against.

### 13.4 What is owed

1. Raise the guest ceiling so a retrying run can finish and report, and derive
   the injector's total budget from it rather than letting SIGTERM arbitrate.
2. Make an arm that never executed say so, distinctly from an arm that failed.

Neither is done at the time of writing: the QEMU host is committed to the
DDR-1002 arm-B campaign, and editing the Makefile under a running campaign is
the contamination R1 exists to prevent.
