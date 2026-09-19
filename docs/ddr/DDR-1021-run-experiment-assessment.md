# DDR-1021 — `ACTION_RUN_EXPERIMENT`: an assessment, not a build

**Status:** ASSESSMENT. **Answer: not buildable as a probe.** No code change.
Closes the Section-3C line item at **6 shipped, 2 deferred with reasons, 0
buildable-and-unbuilt**, and logs both deferrals as
§WHAT "DONE" MEANS requires.

---

## 1. Why this is an assessment

DDR-1020 §6 set the rule that produced this document: *check what exists before
budgeting a type as one more probe.* DDR-1020 §1 records two DDRs of mine that
skipped that check and got the answer wrong from memory. So `RUN_EXPERIMENT` was
grepped first, and the grep is the whole finding.

## 2. What `RUN_EXPERIMENT` requires, and what exists

`docs/AETHER_MASTER_FEATURES.md` §3C specifies it as:

> `ACTION_RUN_EXPERIMENT` (`CAP_EXEC`; metric function lives in a
> `CAP_SOVEREIGN`-locked SFS path — **cannot** be modified by the experimenting
> agent)

Three things are needed. None of the three is present.

| requirement | state | evidence |
|---|---|---|
| `CAP_EXEC` **enforcement** | absent | `CAP_EXEC` is a `#define` in `cap.h:62` and is **checked nowhere**: `grep -rn CAP_EXEC kernel/ --include=*.c` returns **zero** matches. There is no `is_exec` field on `struct tcb`, unlike the `is_memory` (DDR-836) and `is_rewrite` (DDR-842) fields that make `CAP_MEMORY` and `CAP_REWRITE` real. |
| an experiment **subsystem** | absent | `grep -rln experiment kernel/ user/` returns **nothing**. |
| a metric sink the agent may write | absent | The metric lockbox exists (DDR-812) but `SYS_METRIC_READ` (NSI 76) is **`CAP_SOVEREIGN`-only and read-only** (`sys_aether.c:89`). An agent cannot record an experiment result there, which is the design's intent — the metric is the owner's ground truth and must not be writable by the agent being measured. |

## 3. The ruling already exists

This is not a new decision. CLAUDE.md's PRE-APPROVED EXCEPTIONS already log:

- **`CAP_OCR`, `CAP_SCENE` if no hardware path** → *"capability bit defined,
  enforcement deferred — no subsystem path"* — which is exactly `CAP_EXEC`'s
  shipped state, and DDR-982 §2 recorded the same shape for the other bits.
- **`ACTION_EXEC_CODE`** → *"needs sandboxed interpreter subsystem"*.

`ACTION_RUN_EXPERIMENT` sits behind the same missing subsystem: an experiment is
code the agent supplies and the kernel runs under a capability, which is
`ACTION_EXEC_CODE`'s deferral wearing a different name. Building it would mean
shipping a sandboxed interpreter plus a locked metric-write path — a subsystem
and a security-posture decision, not a probe.

**It is not, however, the same as `SEND_IPC`.** `SEND_IPC` has a working
kernel-internal implementation (`ipc_send`/`ipc_recv`) and lacks only a ring-3
door (DDR-1017 §1). `RUN_EXPERIMENT` has no implementation at any ring.

## 4. Both deferrals logged

`ACTION_SEND_IPC` and `ACTION_RUN_EXPERIMENT` were **in the enum but absent from
the exceptions table** — the same gap DDR-982 §5.5 had for the capability bits,
where 17 exceptions were listed and none logged. Both are now logged in
`docs/BUILD_TRACKER.md` with their reasons, so §WHAT "DONE" MEANS's *"zero
unlogged exclusions"* holds for Section 3C.

## 5. A residual worth naming

Both types are **in `enum aether_action`**, so an agent can submit either today
and the kernel will queue it — `SEND_IPC` will even auto-approve in sovereign
mode, since it is not in `aether_action_forces_pending()`. Nothing will then act
on it.

`aether.h` already argues the opposite policy for six other 3C types, which are
deliberately **absent** from the enum:

> *"Declaring an enum value with no enforcement is worse than omitting it: an
> agent could submit one and the kernel would queue an action nothing
> implements."*

By that reasoning these two are on the wrong side of the line. **No change is
made here**, for two reasons: the enum is append-only wire format (DDR-832), so
removing a value renumbers everything after it, and the queue entry is bounded
and audited — it expires via `AETHER_ACTION_TTL_TICKS` and leaves an
`AR_SUBMIT`/`AR_EXPIRE` trail, so the cost is a wasted slot, not an unguarded
action. Recorded as a known inconsistency between `aether.h`'s stated policy and
its contents, for a post-1.0 decision.

## 6. Section 3C, final state

| type | state |
|---|---|
| `READ_FILE` | shipped — DDR-1015 |
| `DELETE_FILE` | shipped — DDR-1016 |
| `SEND_IPC` | **deferred** — no ring-3 IPC surface (DDR-1017 §1) |
| `QUERY_MEMORY` | shipped — DDR-1018 |
| `REWRITE_AGENT_CODE` | shipped — DDR-842 |
| `PROPOSE_HYPOTHESIS` | shipped — DDR-1020 |
| `RUN_EXPERIMENT` | **deferred** — this DDR |
| `EVOLVE_GENOME` | shipped — DDR-1020 |

**6 shipped and gated, 2 deferred with logged reasons, 0 buildable-and-unbuilt.**
