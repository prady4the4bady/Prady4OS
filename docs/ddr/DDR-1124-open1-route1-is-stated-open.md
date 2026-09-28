# DDR-1124 — OPEN-1 ROUTE 1 IS STATED **OPEN**, EXPLICITLY, AND ITS DRAFTED WORDING CARRIED ONE OVERSTATEMENT

**Decision + correction. Docs-only: no code change, no gate, `kernel.bin` NOT
rebuilt. NO FIX, NO MECHANISM NAMED. Route 1 does not close and is not claimed
harmless.**

Date: 2026-09-19 · operator plan step 3 (*"decide OPEN-1 Route 1 explicitly in
release notes/tracker rather than leaving it ambiguous"*)

---

## §1 — THE FIRST FINDING: **THERE ARE NO RELEASE NOTES**

`PRE_LAUNCH_CHECKLIST` §1.2 offers the operator two defensible paths and
describes one of them as *"tagging with route 1 named in **the release notes**
(DDR-1011 §4 already drafted that wording)"*.

**Measured: `find . -iname '*release*'` outside `build/` and `.git/` returns
exactly one path, and it is a DDR filename** — `DDR-1081-group-h-the-release-
table-names-a-gate-a-mechanism-and-an-action-that-do-not-exist.md`. There is no
release-notes file, no `RELEASE.md`, no `CHANGELOG`, nothing.

So DDR-1011 §4's wording has had nowhere to live since the day it was written,
and §1.2 describes a destination that does not exist. **The DDR-1113 §2 /
DDR-1114 §2.2 class** — a row explaining how a non-existent artefact works — and
it is why *"leaving it ambiguous"* was the accurate description: a decision
whose only home is a DDR is a decision nobody consulting the tracker will find.

**This DDR does not create a release-notes file.** Authoring the v1.0.0 release
notes is part of tagging, which is §1.2's operator decision and not this one.
What it does instead is put the statement where the tracker already is:
`PRE_LAUNCH_CHECKLIST` §1.2 and its §2 issue table, plus `CLAUDE.md`'s OPEN-1
row. When release notes are written, **§3 below is the paragraph to copy**, and
it is corrected rather than inherited.

---

## §2 — THE DECISION

**Route 1 of OPEN-1 is OPEN. It is stated as open, by name, in the tracker. It
is NOT closed, NOT deferred, and NOT claimed harmless.**

That is the whole of what this DDR decides, and it is deliberately narrower than
it may look:

| | who decides |
|---|---|
| **how route 1 is stated** | **here** — operator step 3 asked for exactly this |
| whether `v1.0.0` is tagged | **operator**, §1.2, unchanged |
| whether `main` is promoted | **operator**, §1.2, unchanged |

§1.2 says *"an implementer should not make this call"* about **the hold**, and
that sentence stands untouched. Deciding how an open issue is *worded* is not
deciding whether to ship.

---

## §3 — THE STATEMENT, CORRECTED

DDR-1011 §4 drafted five sentences. Four survive. **The fifth is an
overstatement and must not be carried forward.**

It reads:

> *"A continuous SWAPGS-discipline probe now ships in the kernel and **will, on
> the next occurrence, determine whether route 1 shares OPEN-2's cause**."*

**One direction of that holds and the other does not**, and this project has
since measured why, twice:

- **Holds:** a route-1 occurrence carrying `[percpu] gs FAIL` would merge it into
  OPEN-2 and put it behind DDR-1010's root cause. DDR-1011 §4.1 states exactly
  that and it is still right. Route 1's recorded stopping point is inside
  `systest`'s syscall sequence, i.e. squarely on the path that probe covers, so
  it is the right instrument for that direction.
- **Does not hold:** the *absence* of `gs FAIL` would establish nothing of the
  kind. DDR-1119 measured the width — that probe runs at the top of
  `syscall_dispatch`, so its silence is evidence about **syscall entry** and not
  a general exoneration — and DDR-1122 §5 restated it when the probe printed a
  **positive** in a capture whose freeze was on the block path. Meanwhile
  **OPEN-2 has at least five distinguishable producers** (DDR-1019's three,
  DDR-1079's panic-walker path, DDR-1099's `#DB`), of which SWAPGS is **one**. A
  single-producer check cannot answer a five-producer question in the negative.

**The corrected paragraph, which is what release notes should carry:**

> A CI-only hang (OPEN-1 route 1) remains unexplained. It has been observed
> **twice**, both times in CI, stopping inside a fixed window of the boot
> self-test's syscall sequence; the second occurrence panicked without printing a
> register block. Routes 2 and 3 of the same issue are closed on measured
> evidence. Since those observations the diagnostic surface has widened
> considerably — a continuous SWAPGS-discipline probe at syscall entry, a panic
> report that now reaches a CI job log, a `mnt_lock` wait counter, and a per-CPU
> freeze census — **and no further occurrence has been recorded.** A recurrence
> carrying `[percpu] gs FAIL` would merge route 1 into OPEN-2; its absence would
> not separate them.

---

## §4 — WHAT HAS CHANGED SINCE DDR-1011, IN BOTH DIRECTIONS

**The count has not moved, and the window it has not moved across is now much
larger.** DDR-1062 audited **42 CI suites** across 19 SHAs and attributed all
four of its reds elsewhere (`smoke-actiondel` ×2, `smoke-nethammer`,
`smoke-surfclose`) — none a route-1 stop; and every OPEN-2 artefact since
(DDR-1115, DDR-1118, DDR-1120's `[schedcheck]` fires; DDR-1121/DDR-1122's lock
contradiction) is a different signature on a different gate. **No route-1
occurrence has been recorded since DDR-1009 §2.**

**That is still a COUNT, NOT A RATE.** DDR-1011 §5 declined to compute the
denominator — how many suite-runs executed the affected gates — and this
declines it too, for the same reason: the affected gate is `smoke-surfdestroy`
and the second occurrence was on `smoke-msixap`, so the denominator is not the
suite count and deriving one would be the DDR-1042 failure mode, a plausible
number with nothing under it.

**Three instruments now bear on route 1 that DDR-1011 could not cite:**

1. **`mnt_lock` is visible for the first time.** DDR-994 and checklist §4.11 name
   it as *the* prime suspect on route 1's path, and DDR-1047 explicitly could not
   see it (it is not a `spinlock_t`). DDR-1060 §5's `lock_wait_begin`/`end` fixed
   that — and **DDR-1122 §4(b) has now measured it live**: `yield
   lock=g_mounts+0x1C waits=1 waiters=3`, three threads piled on a mount, in a
   real CI capture. **NOT attributed to route 1** (that capture's freeze has its
   own named proximate event) — recorded because the instrument route 1 most
   needs has gone from absent to demonstrated.
2. **DDR-1088**: no CI job log had ever printed a frame of the panic report.
   Route 1's second occurrence *panicked without printing a register block* —
   exactly the failure DDR-1088 fixed. A third occurrence of that shape would now
   be readable.
3. **DDR-1123**: `[apfreeze]` carries the per-CPU census, so a future freeze says
   how many CPUs stopped rather than one by construction (DDR-1122 §2).

**Against: nothing.** No evidence since DDR-1011 argues route 1 is closed, and
none argues it is worse.

---

## §5 — NOT CLAIMED

- **Route 1 IS NOT CLOSED, and is NOT claimed harmless.** Stating an issue
  clearly is not resolving it.
- **NO MECHANISM NAMED, NO FIX**, and §NON-NEGOTIABLE 3 is not disturbed.
- **NO RATE.** Two occurrences is a count; the denominator is deliberately not
  computed, for DDR-1011 §5's reason.
- **NOT merged with OPEN-2.** DDR-1011 §2's refusal to make that inference stands
  verbatim, and §3 above strengthens rather than weakens it.
- **NO release-notes file is created**, and no tagging or promotion decision is
  taken or proposed — §1.2's hold is the operator's and is untouched.
- **DDR-1011 IS NOT WITHDRAWN.** Its analysis, its §2 refusal and its §4.1
  discriminator all stand; **one clause of one sentence** is corrected, and the
  correction is applied **at the site** rather than filed elsewhere (DDR-1110's
  rule).
- **DDR-994 IS NOT CRITICISED.** DDR-1011 already recorded that its *"a hang
  prints nothing"* framing was too strong as stated, and that correction is not
  re-litigated here.
- **NO code change, NO gate run, `kernel.bin` NOT rebuilt**, so the size/headroom
  pair and `ci-docstate-check` are unaffected; `GLOBAL_FORBIDDEN` **77**, **179**
  gates, 79 probe ELFs.
- **No open issue moves** — OPEN-1 stays open on route 1, and OPEN-2 / OPEN-12 /
  OPEN-13 are untouched.
