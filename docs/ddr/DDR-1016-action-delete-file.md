# DDR-1016 — Section 3C `ACTION_DELETE_FILE`, and the ordering DDR-1015 could not measure

**Status:** IMPLEMENTED + gated + M1/M2 mutation-checked on distinct kernel
hashes. Two of eight Section-3C types. Also fixes a latent `_start` alignment
defect DDR-1015 shipped, and names the hygiene-list gap that hid it.

---

## 1. Why this one is a different shape, not a copy

`ACTION_DELETE_FILE` is in `aether_action_forces_pending()` (`aether.h`),
alongside `SPAWN_PROCESS` / `REWRITE_AGENT_CODE` / `EVOLVE_GENOME`. DDR-842 S4
names each as needing a human gate, so it is **never** auto-approved — not even
in sovereign mode, which is the ADR-026 D2 default this kernel boots in. There
is no approver anywhere in a gate boot.

So the correct assertion is the **opposite** of DDR-1015's: the verdict must
stay `AE_PENDING`, and the file must **survive**. DDR-1013 §2.1 recorded that
split in advance precisely so nobody wrote a gate asserting the opposite of
DDR-842's design.

## 2. This measures the ordering DDR-1015 §5 recorded as UNMEASURED

DDR-1015 §5 was explicit that its gate proved the pipeline ran and the read
happened, but **not that the read happened after the approval** — a probe that
read first and submitted afterwards would print an identical line. §5 then named
what would measure it, and predicted the `DELETE_FILE` gate as the natural place.

That prediction holds, for a reason worth stating: **a read leaves no trace and a
delete does.** For `READ_FILE` the two orders are observationally identical. For
`DELETE_FILE` they differ in the filesystem itself — act-then-ask leaves the
target gone. So the ordering becomes measurable here without any new mechanism,
and `keep=1` is that measurement.

## 3. The control, and why `keep=1` is worthless without it

"The target still exists" is evidence only if deletion works on this root in this
boot. A broken `SYS_UNLINK` would make `keep=1` true for the wrong reason, and
would make the M1 mutant pass too — the gate would assert a number and measure
nothing.

So the probe creates a **second** file, `/ADELCTRL`, and deletes it outright with
its own authority, no action involved, through the same `SYS_UNLINK` call the
mutant uses. `ctrl=1` is what licenses reading `keep=1` as a result. The gate
checks `ctrl` **first**, and its failure message says so.

## 4. A force-pending probe cannot busy-poll — measured, not reasoned

The first draft copied DDR-1015's poll loop (up to 20000 iterations, breaking on
the first non-`PENDING` status). The kernel killed the agent:

```
AGENT_RATE_LIMITED PID=37
```

new in that capture and **absent from the `smoke-actionread` baseline**, where
the only `AGENT_RATE_LIMITED` line is the self-test's own deliberate sentinel at
`PID=2742943744` (`AE_TEST_PID`, the DDR-969 non-bug).

`aether_check_rate` (`aether_mem.c:59`) kills any agent exceeding
`AETHER_RATE_MAX = 60` syscalls per `AETHER_RATE_WINDOW = 100` ticks. DDR-1015's
loop is safe **only because an auto-approved action breaks it on the first
iteration**. A force-pending action never breaks it. That is a structural
difference between the two halves of Section 3C, and the remaining three
force-pending types (`SPAWN_PROCESS`, `REWRITE_AGENT_CODE`, `EVOLVE_GENOME`)
will hit exactly this.

**The fix is a ring-3 spin, not a shorter loop.** The wait between the two polls
costs zero syscalls: the thread is still preemptible in a spin — the timer
interrupt does not care that the loop makes no calls — so real time passes and
the sliding rate window drains, for the price of one poll instead of hundreds.

## 5. The `st` arm was dead, and M2 is what proved it

M2 (below) was first run against a probe that polled twice unconditionally. It
failed — but with `ACTIONDEL FAIL: poll2 rc=-3`, not with the `st=2` the gate's
verdict arm was written to catch.

The cause is in `aether_poll` (`aether_queue.c:151-155`): a terminal verdict
(`APPROVED` / `REJECTED` / `EXPIRED`) is latched once and **the slot is freed**.
So the second poll returns `-ESRCH` on any non-pending verdict, the probe fails
there, and **the `st` it printed could only ever be `1`** — a number that looks
like a measurement and cannot vary. `test "$st" = "1"` was unreachable.

Fixed by polling a second time **only if the first said `PENDING`**, and
reporting the first poll's value otherwise. M2 now reports `st=2` and the gate
names the defect in its own words instead of reporting a vanished slot.

This is the same failure mode this repo has now hit four times — a test that
describes the system in its own words instead of asking it — and it is the
reason the mutants are run before the DDR is written, not after.

## 6. Measured

Baseline kernel **`bf6f7c80ed07040f`**, `-Werror` clean, **1,114,506 B** against
the 1,572,864 B gate (+8,192 B over DDR-1015: the page-aligned cost every
embedded probe pays).

```
[actiondel] PRADYOS_ACTIONDEL_OK id=258 st=1 ctrl=1 keep=1
[actiondel] PASS — force-pending, and the unapproved delete did not happen
```

| # | mutation | kernel | result | arm that caught it |
|---|---|---|---|---|
| M1 | probe unlinks the target **before** polling — the agent acting on its own say-so | `4075ae6e2d6015b1` | `st=1 ctrl=1 keep=0` → **FAIL** | `keep` only |
| M2 | kernel drops `DELETE_FILE` from `aether_action_forces_pending()` | `7c86311198e18e7a` | `st=2 ctrl=1 keep=1` → **FAIL** | `st` only |

Three distinct kernel hashes, and **each mutant fails exactly one arm** — so
neither arm is carrying the other. The baseline hash was rebuilt bit-for-bit
after each mutation was reverted.

Gate suite on the baseline kernel, all on one hash (verified before and after
each run):

| gate | result |
|---|---|
| `smoke-actiondel` | PASS |
| `smoke-actionread` | PASS (`n=25 first=P`) |
| `smoke-aether` | PASS |
| `smoke-shell` | PASS (73-pattern forbidden scan clean) |
| `smoke-blkmq` | PASS |
| `smoke-rqstress-liveness` | PASS |
| `smoke-blk-integrity` | PASS |

Static: `ci-shard-check` OK (**160** gates / 10 shards / 7 excluded);
`ci-probe-rodata-check` OK (**63** ELFs); `ci-start-align-check` OK (45 entry
points) — see §7.

## 7. A latent defect DDR-1015 shipped, and the gap that hid it

`ci-start-align-check` failed on this branch, naming **`user/actionreadtest.c`**
— DDR-1015's probe, not this one. Its `_start` lacked
`force_align_arg_pointer`, so the frame sits 8 bytes off and the first aligned
SSE stack access `#GP`s. The check's own message says why "it works today" is not
evidence: the fault may not appear until the probe's code changes.

**Why it was not caught:** CLAUDE.md §HYGIENE GATES lists `ci-shard-check` and
`ci-probe-rodata-check` and **not** `ci-start-align-check`. CI runs all three
(`.github/workflows/ci.yml:35`), and `tools/ci/hygiene_check.sh` runs all three.
The list is the stale copy. **Run `tools/ci/hygiene_check.sh`, not the list** —
it fails loudly and reports each target's rc.

Fixed here. The DDR-1015 commit `8ad4012` and the two after it will show a red
hygiene job in CI for this reason; that red is this defect, and it is fixed on
this tip.

## 8. Six remain

`SEND_IPC`, `QUERY_MEMORY`, `PROPOSE_HYPOTHESIS`, `RUN_EXPERIMENT` follow
DDR-1015's auto-approving shape. `SPAWN_PROCESS`, `REWRITE_AGENT_CODE` and
`EVOLVE_GENOME` follow **this** one and must budget for §4 — their probes cannot
busy-poll either. No claim is made that Section 3C is complete: two of eight are
done, and both shapes now exist as worked examples rather than descriptions.
