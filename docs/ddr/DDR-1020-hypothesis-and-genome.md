# DDR-1020 — `PROPOSE_HYPOTHESIS` + `EVOLVE_GENOME`, and two accounting errors

**Status:** IMPLEMENTED + gated + **five arms, five mutants, each landing on its
own arm**, all on distinct kernel hashes. Corrects the Section-3C tally in
DDR-1017 §7 and DDR-1018 §7. Records an unexplained SFS behaviour, unfixed.

---

## 1. Two corrections to my own accounting

**(a) `ACTION_SPAWN_PROCESS` is not one of the eight.** `aether.h` pins it under
*"Wire-format pins for the pre-existing action types"*, and the DDR-842 3C block
begins at `ACTION_READ_FILE`. DDR-1017's gate for it is real and worth having,
but it does not advance the 3C count — so **"3 of 8" (DDR-1017) and "4 of 8"
(DDR-1018) were both wrong**.

**(b) `ACTION_REWRITE_AGENT_CODE` was already shipped and gated by DDR-842.**
`user/coderewritetest.c` submits it, publishes the id through agent memory, and a
separate *approver* role approves it via `SYS_APPROVE_CODE_REWRITE` (NSI 86);
`smoke-coderewrite` is on shard 7, **strict**. It is the strongest gate of the
set — four capability roles, plus a negative arm proving NSI 86 cannot approve a
*non*-rewrite action, plus the sov-only arm proving `CAP_REWRITE` is not
decoration. DDR-1017 §7 and DDR-1018 §7 each listed it as remaining.

Both errors are the same one: **asserting a type was unbuilt from memory instead
of grepping the tree.** DDR-1018 §1 corrected exactly this for `QUERY_MEMORY` and
I then did it twice more in the same document. The check is one `grep`.

**True tally of the eight 3C types after this DDR:**

| type | state | gate |
|---|---|---|
| `READ_FILE` | shipped | `smoke-actionread` (DDR-1015) |
| `DELETE_FILE` | shipped | `smoke-actiondel` (DDR-1016) |
| `SEND_IPC` | **BLOCKED** — no ring-3 IPC surface exists | — (DDR-1017 §1) |
| `QUERY_MEMORY` | shipped | `smoke-actionquery` (DDR-1018) |
| `REWRITE_AGENT_CODE` | shipped | `smoke-coderewrite` (DDR-842) |
| `PROPOSE_HYPOTHESIS` | **shipped here** | `smoke-actionhypo` |
| `RUN_EXPERIMENT` | not started | — |
| `EVOLVE_GENOME` | **shipped here** | `smoke-actionhypo` |

**6 of 8 shipped, 1 blocked, 1 remaining.**

## 2. One probe, one boot, both sides of the policy split

`PROPOSE_HYPOTHESIS` (10) is *not* in `aether_action_forces_pending()`;
`EVOLVE_GENOME` (12) is. They run in the **same probe in the same boot**, so

```
[actionhypo] PRADYOS_ACTIONHYPO_OK hst=2 hn=33 gst=1 gseed=9 gn=9
```

`hst=2` beside `gst=1` is direct evidence that the force-pending list
*discriminates*. Two separate gates could each have passed for their own
unrelated reasons; this comparison is only available inside one boot against one
policy engine.

The genome arm follows DDR-1016's shape: seed a known genome with the probe's own
authority (`gseed`, the control), propose an evolution, and assert **both** that
the verdict stays PENDING **and** that the bytes on disk are untouched.

## 3. Measured

Baseline kernel **`53fe179c85a7c3b5`**, `-Werror` clean, **1,134,986 B** against
the 1,572,864 B gate.

| # | mutation | kernel | result | arm |
|---|---|---|---|---|
| M1 | probe skips the approved hypothesis log | `35157cf009afe975` | `hn=0` | `hn` |
| M2 | kernel forces `PROPOSE_HYPOTHESIS` pending | `977cbdef336a287f` | `hst=1 hn=0` | `hst` (+`hn`, coupled) |
| M3 | kernel drops `EVOLVE_GENOME` from `forces_pending()` | `ee069299dd4fec34` | `gst=2` | `gst` only |
| M4 | probe evolves the genome on a PENDING verdict | `26236c25931f9fe2` | `gn=17` | `gn` only |
| M5 | genome seed never lands | `f56434e9e879f639` | `gseed=0 gn=0` | `gseed` |

Six distinct hashes. M2's coupling of `hst` and `hn` is by design and not
sloppiness — for an auto-approving type a verdict that never arrives means the
log must not happen, the same property DDR-1018 §5 recorded.

Gate suite on the baseline, one hash verified before and after each run:
`smoke-actionhypo`, `smoke-coderewrite`, `smoke-fsrm`, `smoke-shell`
(73-pattern scan clean), `smoke-blkmq`, `smoke-blk-integrity` — all PASS.
`hygiene_check.sh` ALL THREE PASSED (163 gates / 10 shards / 7 excluded;
66 probe ELFs; 48 entry points).

## 4. M4 caught a gate arm I had already convinced myself was sound

The first M4 **passed**. It evolved the genome by rewriting `/GENOME.TXT` in
place — and the gate reported `gn=9`, unchanged, PASS. I would have shipped the
`gn` arm believing a mutant had proven it.

The cause was not policy. Instrumenting the mutant's return code gave
`put_rc=-1`: **the write itself failed.** A same-length rewrite failed too
(`same_len_rc=-1`), and the file still read back as the original content. Only
**unlink-then-recreate** succeeded (`unlink_rc=0 put_rc=0`), and that is the M4
in the table.

**This is an unexplained SFS behaviour and it is NOT fixed here.** What is
established: on this SFS root, re-opening an existing file `O_CREAT|O_WRONLY` and
writing it again returns short, for both a longer and an equal-length payload,
while `unlink` + create with the same content succeeds. **The ADR-032 write
budget is excluded** — the unlink+create at the very same point in the same boot
succeeded, so writes were still permitted. No mechanism is named beyond that, so
§NON-NEGOTIABLE 3 forbids a fix. Recorded for whoever builds the SFS overwrite
path.

The transferable lesson is about gates, not filesystems: **a mutant that fails to
perform the defect is indistinguishable from a gate that catches it.** M4 must be
verified to have actually done the thing, not merely to have been attempted.

## 5. The dead-arm class, instances four and five — and the general rule

`gseed` was `fail()`ed on before being printed, so it could only ever read `9`.
Then M5 exposed a second layer: even after `gseed` was computed, the *later*
`get` still called `fail()`, so a missing genome killed the probe before the line
printed and took the `gseed` arm with it.

That is instances **four and five** in six DDRs — after DDR-1016 §5 (`st`),
DDR-1017 §4 (`ctrl`) and DDR-1018 §3 (`st`). The pattern is now specific enough
to state as a rule:

> **A probe should REPORT and let the gate JUDGE.** Reserve `fail()` for
> conditions under which no meaningful line can be produced at all. Every
> `fail()` placed before the print removes an arm from the gate, silently — and
> a field whose only reachable value is the passing one is decoration, not
> measurement.

Both were fixed and M5 now lands on `gseed` with the message that names the
right cause: *"the control write did not land, so gn= proves nothing"*.

## 6. What remains

`RUN_EXPERIMENT` is the last unbuilt 3C type. It is **not** obviously buildable
the way the others were: `AETHER_MASTER_FEATURES` §3C specifies `CAP_EXEC` plus a
metric function living in a `CAP_SOVEREIGN`-locked SFS path, and `CAP_EXEC` is
logged in CLAUDE.md's PRE-APPROVED EXCEPTIONS as *"capability bit defined,
enforcement deferred — no subsystem path"*. **Check what actually exists before
budgeting it as one more probe** — that is the §1 lesson, and it applies here
first.
