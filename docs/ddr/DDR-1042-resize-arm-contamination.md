# DDR-1042 — `smoke-resizeall`: one arm failed by another arm's discarded round

**Status:** FIXED + meta-tested (`ci-resizecheck-selftest`, wired into `hygiene_check.sh`)
**Artefact:** CI run 33623855907, shard 9, tip `87321b0`, `smoke-resizeall`.

---

## §1 — The failure, and why it is not what it says

```
[resizeall] FAIL — arm e: E drag did not widen: w0=157 w=150
            line: edge=8 x0=140 y0=140 w0=157 h0=117 x=140 y=140 w=150 h=117
[resizeall] arm s OK   [resizeall] arm w OK   [resizeall] arm n OK
```

**Arm e had succeeded.** Its own drag is in the same log, four lines earlier:

```
PRADYOS_RESIZE_FIX id=1 edge=8 x0=140 y0=140 w0=64 h0=64 x=140 y=140 w=157 h=64
```

64 -> 157, origin held. The record the gate failed on is a *second* `edge=8`
commit, produced by **arm w**.

The injector narrated the whole thing itself:

```
[resize_inject] arm=w start=4708,8458 end=9288,8458
[resize_inject] no RESIZE_TRACK within 20s — compositor never observed the drag; retrying this round
[resize_inject] arm=w round 1 re-resolved start=4708,8458 end=9064,8458
```

Arm w's press at BETA's west handle was missed. But the pointer had already been
dragged to `9288,8458`, which at that moment was BETA's **east** handle
(`rze=9288,8458` in the geometry line immediately above). The compositor saw a
legitimate east grab and committed `157 -> 150`. The retry then performed arm w
correctly, and the gate reported `arm w OK`.

**The compositor is not implicated anywhere in this.** Every `RESIZE_FIX` line in
the capture is self-consistent and holds its fixed edge, including the spurious
one — `x=x0`, `y=y0`.

---

## §2 — The defect: a FIX line does not say which arm produced it

`resize_check.py` infers the arm from the **edge bitmask alone**:

```python
cands = [r for r in recs if (r["edge"] & want[arm]) and not (r["edge"] & excl[arm])]
```

and then required every clause of `check()` to hold for every candidate, on the
reasoning stated in its own docstring:

> a repeated drag is a second independent observation, not a corruption of the
> first.

**That is right for a repeat of the same arm and wrong across arms.** Nothing in
the log distinguishes "arm e ran twice" from "arm w's abandoned round happened to
grab an east edge", so a retry anywhere can inject a foreign record into any
other arm's evidence.

The failure mode is worse than a flake: it produces a **specific, plausible,
false accusation** against a subsystem that behaved correctly, with a real log
line to back it up.

---

## §3 — The fix: invariant for all, liveness for one

`check()` was doing two different jobs under one name:

| clause | what it is |
|---|---|
| `x == x0 && y == y0` (E/S), `x+w == x0+w0` (W), `y+h == y0+h0` (N) | **DDR-997's actual invariant** — the fixed edge held. A property of the compositor. What M1 and M2 break. |
| `w > w0` (E), `h > h0` (S), `w == 32 && x != x0` (W), `h == 32 && y != y0` (N) | **liveness** — the injector managed the drag it intended. A property of the *harness*. |

Split into `invariant()` and `liveness()`:

- **`invariant()` must hold for EVERY record** carrying the arm's edge bit,
  whoever produced it. A resize that moves the edge it is supposed to pin is
  wrong regardless of which arm asked for it, so no mutation coverage is lost.
- **`liveness()` need hold for at LEAST ONE.** A foreign record cannot fail it,
  and an arm that never really ran still cannot pass it — there would be no live
  observation at all.

The output now reports both counts (`1 live of 2 observation(s)`), so a
contaminated run is visible rather than silently tolerated.

---

## §4 — Measured, without QEMU

The 17 `PRADYOS_RESIZE_FIX`/`REQ`/`GEOM` lines were lifted verbatim from the CI
job log into a fixture. No boot is needed to test a log parser, and doing it this
way means the regression test is the **actual failing artefact**, not a
reconstruction of it.

| fixture | old checker | new checker | why it exists |
|---|---|---|---|
| `resize_crossarm_pass.log` (the real CI capture) | **rc=1** | **rc=0** | the bug |
| `resize_m1_w.log` — W drag, right edge moves | rc=1 | **rc=1** | M1 still caught |
| `resize_m2_e.log` — E drag, origin moves | rc=1 | **rc=1** | M2 still caught |
| `resize_dead_e.log` — no widening E record at all | rc=1 | **rc=1** | a dead arm still cannot pass |

**The three negative fixtures are the load-bearing half.** Without them, "made
the failure go away" and "fixed the checker" are indistinguishable — and making
the failure go away is exactly what a narrower fix (drop the `w > w0` clause)
would have done.

End to end: `make smoke-resizeall` rc=0 locally, all four arms `1 live of 1`
(a healthy run never retries, so contamination does not arise).

---

## §5 — `ci-resizecheck-selftest`, and why it goes in `hygiene_check.sh`

`resize_check.py` decides whether `smoke-resizeall` passes. **No amount of
running that gate could have found this defect** — the gate ran, produced a
failure, and the failure named the wrong component. A checker needs its own
tests, the same argument `smoke-selftest` case 5 rests on (DDR-791).

It is added to `tools/ci/hygiene_check.sh` rather than to a list of target names
in `CLAUDE.md`, per that file's own item 2: *"A list of names drifts; the script
cannot."* The script now runs four checks and says `ALL FOUR PASSED`.

---

## §6 — Attribution: NOT established, and not claimed

The failing tip was `87321b0` (DDR-1040, SMEP). Two things are true and neither
settles it:

- **The checker defect predates it.** It is in the arm-partitioning logic and is
  triggered by an injector retry, which has nothing to do with SMEP.
  `smoke-resizeall` on shard 9 has failed in CI before, on an earlier tip.
- **DDR-1040 is not literally zero perturbation on the CI CPU model.** SMEP is
  absent there, so `cpu_enable_smep()` returns at the CPUID check — but
  `smep_selftest` still creates and destroys an address space and prints three
  serial lines during boot, and this gate is timing-sensitive enough that a
  missed press is its known failure mode.

So: whether DDR-1040's boot perturbation made the retry more likely is **not
established**, and this DDR does not claim it either way. What is established is
that the compositor was correct and the checker was not, which is what needed
fixing regardless.

## §7 — What this does not fix

- **The injector can still miss a press**, and still drags the pointer to the
  target coordinate before discovering it. The retry recovers, but the abandoned
  round still commits a real resize on whatever edge the pointer landed on. That
  is a harness defect with a real cost (a spurious resize), left because the
  repair is in `resize_inject.sh`'s press-confirmation, which is a larger change
  than this failure justifies.
- **Cross-arm contamination is contained, not eliminated.** A foreign record that
  happened to be *live* for another arm would still be accepted as that arm's
  evidence. It would have to hold the invariant to do so, so it cannot mask a
  compositor defect — but the gate would be reporting an arm it did not run.
  Naming it because the count in the output (`N live of M`) is now the only place
  a reader can see it.
