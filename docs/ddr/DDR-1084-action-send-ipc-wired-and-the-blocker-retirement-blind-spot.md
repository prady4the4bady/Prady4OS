# DDR-1084 — Section 3C `ACTION_SEND_IPC` wired: Section 3C CLOSES, a pin arrives with a true justification, and a structural blind spot named

**Date:** 2026-09-07
**Status:** IMPLEMENTED + gated + M1/M2
**Branch:** `dev/phase1-seyp3n`
**Supersedes in part:** DDR-1017 §1 (the "not buildable at any ring" verdict)
**Answers:** DDR-1072 §2's second trapped row; completes DDR-1083

---

## §1 — THE STRUCTURAL FINDING: a DDR that retires a blocker does not, by default, revisit the row it was blocking

This is the **second consecutive** instance and the pattern is worth naming
rather than fixing twice in silence.

| row | blocker recorded | who retired it | the retiring DDR's own NOT CLAIMED | row stayed open |
|---|---|---|---|---|
| `ACTION_RUN_EXPERIMENT` | DDR-1021: "no experiment subsystem exists" | **DDR-1034** | *"NOT DONE: the AETHER action path does not yet CALL this"* | ~49 DDRs (→ DDR-1083) |
| `ACTION_SEND_IPC` | DDR-1017: "no ring-3 door — no executor in any ring" | **DDR-1033** | *"NOT DONE: the AETHER action path does not yet CALL this, so an approved SEND_IPC still has no automatic effect"* | ~51 DDRs (→ here) |

Both times the unblocking DDR **stated the residual accurately in its own text**
and the backlog row was not updated, because the session that removed the
blocker was working on the *subsystem*, not on the *row*. That is DDR-1071 §4's
reading — *"correction is a side effect of adjacent work rather than a process"*
— applied to **blockers** rather than to completion markers, and it is the more
expensive direction of the two: a stale completion marker understates progress,
while **a stale blocker suppresses work that is in fact unblocked** (DDR-1072 §5
named that asymmetry for the agent-respawn row; this is the same shape twice
more, in the same table).

**Not built: a checker.** The signal would be "a row's stated blocker is no
longer true", which is semantic — the same wall DDR-1071 §5 and DDR-1072 §2 both
hit, and DDR-1081 §3 sharpened with a case where the identical mechanical signal
was a defect on one row and correct on three others. What is available instead is
cheap and is done here: **when a DDR retires a blocker, it names the rows that
blocker was holding.** DDR-1033 and DDR-1034 each named the residual in prose;
neither named the row.

---

## §2 — The same pin rule as DDR-1083, arriving CORRECTLY this time

`kernel/aether/aether.h` states its pinning rule at lines 63-67, for this exact
type:

> *"NOTE `ACTION_SEND_IPC == 7` sits between these two and is deliberately NOT
> pinned … Nothing hand-copies 7, and **a pin whose probe does not exist would
> read as a claim that one does**."*

That was **true when written and true until this commit** — measured:
`grep -rn ACTION_SEND_IPC` over `.c`/`.h` outside the header returns exactly two
lines, both *comments* (`user/actionquerytest.c:4`, `user/ipctest.c:3`), neither
a constant.

`user/actionipctest.c` hand-copies 7 and submits the type, **so the pin becomes
owed and is added** — with a justification that is accurate at the moment it is
written. One commit after DDR-1083 §2 found the same file's *other* pin carrying
a justification that was false, the rule's condition is satisfied properly:
same file, same rule, opposite outcome, and the difference is only whether the
probe was built before or after the sentence claiming it.

*(Unchanged, and noted in DDR-1083 §2: `ACTION_NET_EGRESS == 13` is pinned and
hand-copied by nothing. Its justification is genuinely different — it crosses the
boundary inside every **audit record**, not every submission — and the comment
does not say so. Still not touched; a comment edit on a correct pin.)*

---

## §3 — WHAT THIS DOES AND DOES NOT MEAN — the same statement DDR-1083 §3 makes, for the same reason

`sys_ipc_send` (`sys_aether.c:342`) checks `is_ipc` and `ipc_send`'s
`cap_authorize` and **does not consult the action queue**. This does not make it.

- **NOT ADDED:** any new enforcement. An agent holding `is_ipc` + the RES_IPC
  capability can call NSI 98 directly, without submitting, before and after.
- **ADDED:** the propose → arbitrate → **obey** loop and its audit record.

That is the system's design for every action type (DDR-1013 §2). Stated again
rather than cross-referenced, because "the kernel now gates IPC" is the available
false reading and it is the DDR-1059 shape.

---

## §4 — The obvious arm is vacuous, measured before it was written

`smoke-sendipc` **already** asserts the door: `user/ipctest.c` sends to slot 2
and the gate requires the resulting `rc=0`. So *"submit ACTION_SEND_IPC, then
assert the send returned 0"* passes on a build where the submit is deleted
outright. Sixth time caught in design text (DDR-1039 §3.1, 1058 §2, 1067 §2,
1070 §4, 1083 §4, here).

The message CONTENT does not rescue it either: the probe holds the four words it
sent, so printing them back proves nothing an agent could not compute without
sending — DDR-1066's M2 lesson exactly.

---

## §5 — ARM B IS STRONGER HERE THAN IN DDR-1083, AND THAT IS THE DESIGN

DDR-1083's arm B prints `ran=0` — a flag the probe reports **about itself**, and
its own §9 recorded arm A as the weaker arm for precisely that reason. Here the
decline is **confirmed by the kernel**:

```
arm B: parent = a force-pending action  ->  child RUN stays AE_PENDING
       probe DECLINES to send
       probe then calls SYS_IPC_RECV on that slot
       kernel finds the endpoint EMPTY and returns -ETIMEDOUT (-110)
```

`-110` is a value the probe **cannot manufacture without the kernel having
searched a genuinely empty endpoint** — the `-ENOENT`-you-cannot-fake shape
DDR-1066 established, and the property DDR-1083 §9 explicitly said arm A lacked.
A probe that wrongly sent despite the PENDING verdict leaves `e->full = 1`, and
the same receive returns **0** instead.

**The cost is real and is stated, not hidden:** `ipc_recv` blocks for DDR-961's
500-tick bound before reporting `-ETIMEDOUT`, so arm B pays a bounded wait inside
the gate's 120 s window. It costs no syscall budget (the process blocks in the
kernel; `AETHER_RATE_MAX` counts syscalls, not time), which matters because this
probe holds `is_agent`.

---

## §6 — Design: one probe, one boot, two arms

`user/actionipctest.c`, a **third** process under the existing `ipc` probe key —
not a flag on the two `ipctest` spawns, for DDR-1083's reason: it needs
`is_agent` (to submit at all) **and** the door, and `IPCDENY` exists precisely to
lack the door.

**Arm A — approved, so it sends.**
Plain `SYS_SUBMIT_ACTION(ACTION_SEND_IPC, …)` → auto-approves (sovereign, not
force-pending, no parent) → `SYS_IPC_SEND(slot 4)` → `SYS_IPC_RECV(slot 4)`
returns 0 and the words come back:

```
PRADYOS_IPCACT_A st=2 sent=1 rc=0 back=0xA71C0001
```

**Arm B — pending, so it declines, and the kernel says the slot is empty.**
`ACTION_DELETE_FILE` parent (force-pending, PENDING forever) →
`SYS_SUBMIT_CHILD_ACTION(ACTION_SEND_IPC, …, parent)` → PENDING → probe does
**not** send → `SYS_IPC_RECV(slot 5)` → `-ETIMEDOUT`:

```
PRADYOS_IPCACT_B st=1 pst=1 sent=0 rc=-110
```

**Slots 4 and 5, measured disjoint:** `ipctest.c:23` uses `SLOT 2` (and 99 as its
out-of-range arm), `AGENT_ROSTER_N` is 8. The two probes co-boot under one key
and must not share an endpoint, or arm B's emptiness claim would be about
`ipctest`'s traffic rather than its own.

Parent type is immaterial (only membership in `aether_action_forces_pending()`
is load-bearing); nothing executes `DELETE_FILE` and the probe never acts on it.

`aether_action_forces_pending()` is **not changed** — SEND_IPC auto-approves in
sovereign mode, as it did before this commit. Whether it belongs in that list is
a DDR-842 S4 **policy** decision, the DDR-793/982 class deferred to the operator.
Recorded; not taken.

**NO NEW GATE (178 unchanged).** The arms go on `smoke-sendipc`, which
**resolves the DDR-1072 §2 trap for this row** — that gate's name matched the
action type while its claim was the door; it now covers both. With DDR-1083 the
same trap is closed on `smoke-runexp`, so **both** traps DDR-1072 §2 named are
gone and **Section 3C closes at 8 of 8 shipped and gated**.

---

## §7 — Mutants, landing on different arms

| Mutant | Defect | Fails |
|---|---|---|
| **M1** | Arm A submits `ACTION_DELETE_FILE` (force-pending) — the wire-drift class §2's pin exists to prevent | Arm A — `st=1 sent=0`; arm B correct |
| **M2** | Arm B sends regardless of the verdict | Arm B — the slot is full, so the receive returns **0** not `-110`; arm A correct |

M2 is the load-bearing one: the discriminating value comes from the kernel.

---

## §8 — NOT CLAIMED

- **No new enforcement** (§3). `sys_ipc_send` still does not consult the queue.
- **No policy change.** `aether_action_forces_pending()` untouched.
- **No kernel defect is fixed and none is alleged** — `ipc_send`, `ipc_recv`,
  `ipc_grant`, the policy engine and the DAG were all correct; what was missing
  is a **caller**, as DDR-1033 itself said.
- **Section 3C closing is about the ACTION TYPES only.** Six of the eight were
  already shipped (DDR-1072 §1); the two remaining are now wired. It says nothing
  about the Group F domain agents (F#66/67/69-75), which remain unbuilt.
- **§1 names a pattern from two instances.** Two is not a rate, and no checker is
  built — the signal is semantic, for the reason DDR-1071 §5 / DDR-1072 §2 /
  DDR-1081 §3 each measured independently.
- **The `ACTION_NET_EGRESS` pin is noted, not touched** (§2).
- **`GLOBAL_FORBIDDEN` 76 unchanged; no new gate (178 unchanged); no open issue
  moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.
