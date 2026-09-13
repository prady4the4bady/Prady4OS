# DDR-1083 — Section 3C `ACTION_RUN_EXPERIMENT` wired, and a wire-format pin whose stated justification was false

**Date:** 2026-09-07
**Status:** IMPLEMENTED + gated + M1/M2
**Branch:** `dev/phase1-seyp3n`
**Supersedes in part:** DDR-1021 §1 (the "not buildable at any ring" verdict)
**Answers:** DDR-1072 §2's "a reason to re-assess, not to close"

---

## §1 — DDR-1021 named three blockers. All three are gone, measured.

DDR-1021 assessed `ACTION_RUN_EXPERIMENT` as **not buildable at any ring** on three
facts. Each was true when written. Each is now false, and none of them was
retired by this DDR — they were retired by DDR-1034, which built the subsystem
and then did not come back to the action type.

| DDR-1021's blocker | State today | Measured at |
|---|---|---|
| "`CAP_EXEC` is a `#define` checked NOWHERE" | Minted and `cap_authorize`d | `kernel/syscall/sys_experiment.c:35`, `:45` |
| "no experiment subsystem exists" | Bounded stack machine, CI-gated | `kernel/aether/experiment.c`, `smoke-runexp` (shard 8, strict) |
| "the metric lockbox is `CAP_SOVEREIGN` read-only, so the agent being measured cannot write its own result" | Solved a different way, without touching the lockbox | `sys_experiment.c:11-14` — the results store's only writer is `exp_run()`, in the kernel |

The third is the interesting one: DDR-1021 read the lockbox's read-only property
as a *blocker*, and DDR-1034 reproduced the same property by a different
mechanism rather than working around it. So the blocker did not need lifting; it
needed a second implementation of what it was protecting.

**CLAUDE.md's own DDR-1072 entry already said this was owed**: *"DDR-1021
deferred the ACTION TYPE and DDR-1034 removed part of that blocker by building
the subsystem — a reason to **re-assess**, not to close."* This is that
re-assessment, and the answer is: buildable.

---

## §2 — THE FINDING: a `_Static_assert` whose stated reason is false, three lines below the comment that gives the opposite rule

`kernel/aether/aether.h` is the wire-format authority for the action enum, and it
states its own pinning rule twice. For `ACTION_SEND_IPC` (lines 63-67):

> *"NOTE `ACTION_SEND_IPC == 7` sits between these two and is deliberately NOT
> pinned … **Nothing hand-copies 7, and a pin whose probe does not exist would
> read as a claim that one does.**"*

Eleven lines later (73-77), for `ACTION_RUN_EXPERIMENT`:

> *"11 (`RUN_EXPERIMENT`) WAS deliberately unpinned on that rule — nothing copied
> it, and a pin whose probe does not exist reads as a claim that one does.
> **DDR-1034 built that probe (`user/exptest.c`), so the pin is now owed** and is
> below; this comment is updated in the same commit rather than left to become
> false."*

**`user/exptest.c` does not hand-copy `ACTION_RUN_EXPERIMENT`. It never has.**
Measured, not inferred:

```
$ grep -rn 'ACTION_RUN_EXPERIMENT' --include=*.c --include=*.h --include=*.asm . \
      | grep -v 'kernel/aether/aether.h'
./kernel/aether/experiment.h:1:/* kernel/aether/experiment.h — DDR-1034: ACTION_RUN_EXPERIMENT.

$ grep -nE 'ACTION_|SUBMIT_ACTION|POLL_RESULT' user/exptest.c
(no output)

$ grep -nE '^#define SYS_' user/exptest.c
18:#define SYS_EXIT            4
19:#define SYS_WRITE           6
20:#define SYS_RUN_EXPERIMENT 100
21:#define SYS_EXP_RESULT     101
```

The one match outside the header is a **filename comment**. `exptest.c` copies
four NSI numbers and the `exp_op` opcodes; it never touches the action enum and
never calls `SYS_SUBMIT_ACTION`. It drives the **executor**, which is precisely
what DDR-1072 §2 established when it warned that `smoke-runexp`'s name matches
the action type while its claim is a different thing.

So the file contains, eleven lines apart, a rule and a violation of that rule —
and the violation is written in the confident past tense, so a reader checking
it finds a claim about a probe that does not do what the comment says. Same class
as DDR-1073 §5 (a prescribed remedy that was never runnable) and §INV.12's wrong
reason behind a right conclusion, and it sits in the file whose entire job is to
be the thing other files are checked against.

**The pin is not deleted. It is made true.** This DDR ships
`user/actionexptest.c`, which *does* hand-copy 11, so the sentence becomes
accurate rather than the comment becoming a smaller claim. Deleting the pin was
the alternative and is worse: the number genuinely crosses the ring boundary the
moment any probe submits the type, and this DDR is that moment.

*(Recorded, not acted on: `ACTION_NET_EGRESS == 13` is likewise pinned and
likewise hand-copied by nothing — DDR-1070 made it audit-only on purpose, so
nothing submits it. That pin's justification is a different one (it crosses the
boundary inside every **audit record**, not every queue submission), which the
comment does not state. Not touched here; it would be a comment edit on a
correct pin, and this DDR already has one finding in this file.)*

---

## §3 — WHAT THIS DOES AND DOES NOT MEAN. Stated first, because the wrong reading is available and flattering.

`sys_run_experiment` checks `is_exec` and `cap_authorize`, and **does not consult
the action queue**. This change does not make it. So:

- **NOT ADDED:** any new enforcement. An agent holding `is_exec` + `CAP_EXEC` can
  call NSI 100 directly today, without submitting anything, and can still do so
  after this commit.
- **ADDED:** the propose → arbitrate → obey loop and its audit trail. The agent
  proposes, the kernel records and rules, and the agent acts only on approval.

That is not a weakness peculiar to this type — **it is the system's design for
every action type it has**, and DDR-1013 §2 says so outright: *"the kernel is the
policy engine and the AGENT executes after approval — there is no kernel executor
for `ACTION_WRITE_FILE` either."* DDR-1066's fix was to make the agent actually
perform the work after approval, not to move the work into the kernel. The real
enforcement layer is capabilities and the S-invariants; the action queue is
arbitration and audit.

Writing it down because the false version — *"the kernel now gates experiments"* —
is one sentence away and would be the DDR-1059 shape: a control that reads
stronger than it is.

---

## §4 — THE OBVIOUS GATE ARM IS VACUOUS, AND THAT IS MEASURED BEFORE IT IS WRITTEN

The natural arm is *"submit the action, then assert the experiment computed 42."*
It proves nothing. `smoke-runexp` **already** requires that value, today, with no
action involved anywhere — from the Makefile recipe verbatim:

```
EXTRA_SENTINEL="$(printf 'PRADYOS_EXP_CALC rc=0 v=42\n...')"
```

So an arm of that shape passes on a build where the submit is deleted outright.
Fifth time this project has caught a vacuous arm in design text rather than after
shipping it (DDR-1039 §3.1, DDR-1058 §2, DDR-1067 §2, DDR-1070 §4, here).

**What only the verdict path can produce is the DECLINE.** An arm asserting that
the probe *ran* is satisfied by a probe that ignores the verdict; an arm asserting
that the probe *did not run, and said so*, is not.

---

## §5 — Producing a non-approved verdict deterministically

`AE_REJECTED` is **not reachable in a gate boot**: `grep -rn 'AE_REJECTED'
--include=*.c kernel/` returns two lines, both in `aether_queue.c`, and the only
writer is `aether_decide(id, approve=0)` — an operator action. No gate approves or
rejects anything.

`AE_PENDING` is reachable and deterministic, via DDR-839's DAG
(`aether_queue.c:117-122`, read in full):

```c
int parent_ready = (parent_action_id == 0);
if (!parent_ready) {
    struct aether_action_entry *par = entry_of(parent_action_id);
    parent_ready = (par && par->status == AE_APPROVED);
}
if (!aether_action_forces_pending(action_type) && g_sovereign_mode && parent_ready) {
```

So a child of a **force-pending** parent stays PENDING for as long as the parent
does, which in a gate boot is forever.

**Parent choice, recorded because it looks arbitrary and is:** `ACTION_DELETE_FILE`.
Its *type* is immaterial — the only load-bearing property is membership in
`aether_action_forces_pending()`. Nothing in the kernel executes DELETE_FILE
(DDR-1013 §2), the probe never acts on it, and no approver exists in a gate boot,
so it stays a queue entry and an audit record and nothing else.

**Considered and rejected:** a *bogus* parent id, one syscall cheaper and equally
deterministic (`entry_of` returns NULL → `parent_ready = 0`). It exercises the
"parent missing" branch instead of the "parent not yet approved" branch, and the
second is the realistic one and the one DDR-839's DAG is actually about.

---

## §6 — `aether_action_forces_pending()` is DELIBERATELY NOT CHANGED, and if the operator disagrees that is theirs to say

`ACTION_RUN_EXPERIMENT` is **not** in the force-pending set, so in sovereign mode
it auto-approves. `ACTION_REWRITE_AGENT_CODE` and `ACTION_EVOLVE_GENOME` **are**,
and the superficial argument for adding this one beside them is that all three are
"the agent running agent-authored things."

It is not built, for a reason measured in `experiment.h`'s own header rather than
argued:

> *"THE SECURITY ARGUMENT IS THE INSTRUCTION SET, NOT A GUARD. There is no LOAD,
> no STORE and no addressing mode … DIV is absent rather than guarded."*

An approved experiment computes an integer and can touch nothing — no memory
outside its own 32-slot operand stack, no filesystem, no device, bounded at 4096
retired instructions. `REWRITE_AGENT_CODE` and `EVOLVE_GENOME` change the agent
itself. Those are different in kind, not in degree.

And more importantly: **adding a type to that list is a policy change**, DDR-842
S4's human gate, which this project defers to the operator (the DDR-793 /
DDR-982 class). Recorded here so the decision is visible and not silently made by
whoever wires the type. **No change taken.**

---

## §7 — Design: one probe, one boot, two arms

`user/actionexptest.c`, modelled on `user/actionhypotest.c` (DDR-1020) — one
probe running both sides of one policy engine in one boot, because two probes
could each pass for their own unrelated reasons.

**Arm A — approved, so it runs.**
`SYS_SUBMIT_ACTION(ACTION_RUN_EXPERIMENT, prog, len)` → auto-approves (sovereign,
not force-pending, no parent) → probe calls `SYS_RUN_EXPERIMENT` → prints

```
PRADYOS_EXPACT_A st=2 ran=1 rc=0 v=42
```

**Arm B — pending, so it declines.**
`SYS_SUBMIT_ACTION(ACTION_DELETE_FILE, …)` → id `P`, PENDING forever →
`SYS_SUBMIT_CHILD_ACTION(ACTION_RUN_EXPERIMENT, prog2, len, P)` → parent not
approved → PENDING → probe does **not** call `SYS_RUN_EXPERIMENT` → prints

```
PRADYOS_EXPACT_B st=1 ran=0 rc=0 v=0
```

**Arm B's program computes a DIFFERENT value (97, as `10*10 - 3`) from arm A's
42**, so a spurious run is visible in the value stream and not only in the flag.
`v=97` is added as a `FORBIDDEN_SENTINEL` — free here, because `smoke-runexp`
already declares one (`EXPTEST FAIL`), so per DDR-1043 the gate is already never
early-exit eligible and there is no exit to lose.

The probe **reports and the gate judges** (DDR-1020's rule): every field is
printed unconditionally and no `fail()` precedes a print, so no arm can be
silently removed by an early exit.

Polls are **bounded** (a handful, with ring-3 spins between), not DDR-1015's
20000-iteration loop: this probe holds `is_agent`, so `AETHER_RATE_MAX` (60
syscalls / 100 ticks) applies and an unbounded poll on a PENDING action is killed
with `AGENT_RATE_LIMITED` before anything prints — the defect DDR-1016 §4 hit and
measured. Total syscall count is ~12.

**NO NEW GATE (178 unchanged).** The arms go on `smoke-runexp`, under the same
`QEMU_PROBES=exp` key, for the reason DDR-1039 recorded for `smoke-readline` and
DDR-1070 for `smoke-privacy-netfilter`. This also **resolves the DDR-1072 §2 trap
for this row**: that gate's name matched the action type while its claim was the
executor; after this commit the claim covers both, and the Group F row is updated
in the same commit to say so rather than left to be re-discovered.

---

## §8 — Mutants, landing on different arms (the DDR-1044 M2/M3 check)

| Mutant | Defect | Fails |
|---|---|---|
| **M1** | Arm A skips the submit and runs the program directly | Arm A — no action id, `st=` is not 2 |
| **M2** | Arm B runs regardless of the verdict | Arm B — prints `ran=1`, and `v=97` trips the forbidden sentinel |

Neither carries the other: M1 leaves arm B correct, M2 leaves arm A correct.

---

## §9 — NOT CLAIMED

- **No new enforcement.** §3. `SYS_RUN_EXPERIMENT` still does not consult the
  action queue, and an `is_exec` agent can still call it without submitting.
- **`ACTION_SEND_IPC` is NOT wired** and is not re-assessed here. It is the same
  shape (DDR-1033 built the door; the action path does not call it) and it is a
  separate change; the checklist §4.1 entry stands.
- **No policy change.** §6 — `aether_action_forces_pending()` is untouched, and
  whether RUN_EXPERIMENT belongs in it is recorded as an operator matter.
- **No kernel defect is fixed and none is alleged.** The policy engine, the DAG,
  the executor and the capability check were all correct; what was missing is a
  caller.
- **The `ACTION_NET_EGRESS == 13` pin is not touched**, only noted (§2).
- **No open issue moves.** OPEN-1/2/12/13 untouched. Not an apfreeze, not OPEN-2.
- **`GLOBAL_FORBIDDEN` 76 unchanged**; no new gate (178 unchanged).
