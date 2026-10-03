# DDR-1155 — U1/U2/U3 residual hunt signatures: factual record and the test owed

**Status: RECORD + one test PENDING. No open issue closes. No mechanism is
named.** Committed per the operator instruction (2026-10-03) to "write a short
factual record first, then the test as a standard KASAN use-after-free
regression test of our own kernel. If a piece is blocked, commit the rest and
mark it PENDING."

## §1 What the three signatures are

Across the OPEN2_HUNT campaign (DDR-1154) and the dispatches that preceded it,
three one-off capture shapes were seen that are not the double-dispatch defect
DDR-1139 named and fixed. They are labelled U1, U2, U3 for reference only;
labelling is not attribution.

- **U1** — a single `#GP` whose faulting RIP resolved to `resolve+0x61`
  (`kernel/cap.c`, the static `resolve()` of DDR-1133 §6). Seen **once**.
- **U2** — recorded in the DDR-1134 family: a silent-loser `#UD` at a RIP
  inside a thread's kernel stack. Seen once in that dispatch.
- **U3** — recorded in the DDR-1134/1136 family: a lane-12-shaped panic whose
  body the then-current hunt printer dropped (the DDR-1135 printer defect, since
  fixed).

## §2 Why none of the three is closed, stated as a limitation

- A `#GP` carries **no faulting address** (unlike a `#PF`, which leaves CR2).
  So the single U1 capture fixes the RIP of the faulting instruction and says
  nothing about *which* pointer was bad. One sample cannot rule out that some
  other garbage pointer, not the one hypothesised in §3, produced it.
- U2 and U3 are each **n=1** with the body partly or wholly unread at the time
  (U3's body was dropped by the printer DDR-1135 fixed). A later recurrence
  would now be readable; none has occurred.
- **None has recurred.** At the time of writing the merged-tree campaign stands
  at **5,445 read boots** on the pinned binary `182c30bb16930d57` with **0**
  signals of any kind (DDR-1154 §6.4). That is a strong *absence* and not a
  resolution: NON-NEGOTIABLE 3 forbids a fix without a named mechanism from a
  real failing artefact, and there is no live artefact for any of U1/U2/U3.

## §3 The U1 hypothesis (hypothesis only — NOT a finding)

`resolve()` walks a capability table. A `#GP` on a plain load there is
consistent with a **non-canonical** pointer (DDR-1079's reasoning: a bad-but-
canonical address faults `#PF`, a non-canonical one faults `#GP`). One way a
table pointer becomes non-canonical is a freed-then-recycled control block whose
memory the KASAN-style poison (`KASAN=1`, Makefile) has stamped, read before the
field is re-initialised. **This is a hypothesis, consistent with the one
sample, and is not claimed.** It is written down so the test in §4 is aimed at a
stated target rather than at nothing, and so a future reader does not mistake
the single capture for a diagnosis.

## §4 The test owed — PENDING

The operator asked for a standard KASAN use-after-free **regression test of our
own kernel**: deliberately exercise the recycle path §3 describes under
`KASAN=1`, assert the poison is detected, and mutation-check that a build which
skips the re-initialisation is caught while a correct build is not.

**This piece is PENDING.** It is not committed in this DDR. The authoring of
the test body has been repeatedly interrupted before completion; it is recorded
here as owed so the record is honest and the rest of the DDR is not held up,
exactly as the operator instruction allows. When it lands it will be its own
gate (the `smoke-*` KASAN regression arm) with its before/after mutant, and this
section will be replaced with the measured result.

## §5 Not claimed

- U1, U2 and U3 are **NOT closed** and no open issue (OPEN-1/2/12/13) moves.
- No mechanism is named for any of the three; §3 is a hypothesis.
- No code change ships in this DDR. `kernel.bin` is not rebuilt.
- The 5,445-boot absence is reported as an absence, not as a refutation of any
  of the three occurrences, which stand as recorded in their own DDRs.
