# DDR-1072 — The Group F table: six rows present shipped, CI-registered work as
# remaining; the two that are genuinely open sit beside gates whose NAMES match
# them; and four more are a recorded refusal

**Date:** 2026-09-06
**Status:** measured. **Docs only — no code change, `kernel.bin` untouched.**
**Class:** DDR-1063 §7b (a live-state row that stopped tracking the tree), third
instance after DDR-1063 itself and DDR-1071 — plus the DDR-1068 §2 / DDR-1070 §6
class (a row listing a recorded refusal as remaining work), instances three
through six.

---

## 1. What was measured, and against what

The Group F table in `CLAUDE.md` had not had the audit Groups C, D and E each
received in the last three sessions. It was measured against the **Makefile**
and **`tools/ci/gate_shards.txt`** — never inferred from a DDR, per DDR-1007's
discipline — and, for the action types, against the **probe sources**, because a
gate's name is not its claim.

### 1.1 Six Section 3C rows are shipped, gated and CI-REGISTERED

All eight Section 3C rows carry `— | gate per type`: no gate name, no completion
marker. Six of the eight have a ring-3 probe that submits **that exact action
type** and a gate that is in a shard and not excluded:

| action type | probe | gate | Makefile | shard | tier |
|---|---|---|---|---|---|
| `ACTION_READ_FILE` | `user/actionreadtest.c` | `smoke-actionread` | 3680 | 1 | fast |
| `ACTION_DELETE_FILE` | `user/actiondeltest.c` | `smoke-actiondel` | 3793 | 1 | fast |
| `ACTION_QUERY_MEMORY` | `user/actionquerytest.c` | `smoke-actionquery` | 3861 | 6 | fast |
| `ACTION_REWRITE_AGENT_CODE` | `user/coderewritetest.c` | `smoke-coderewrite` | 2869 | 7 | **strict** |
| `ACTION_PROPOSE_HYPOTHESIS` | `user/actionhypotest.c` | `smoke-actionhypo` | 3893 | 3 | fast |
| `ACTION_EVOLVE_GENOME` | `user/actionhypotest.c` | `smoke-actionhypo` | 3893 | 3 | fast |

`shard_check.sh:50` defines the whole exclude set — `smoke-aarch64
smoke-riscv64 smoke-agent-live smoke-selftest smoke-fs-liveness smoke-fast` —
and none of these six is in it. **Registration, not existence, is the claim:**
all six run on every CI suite, one at strict tier.

This is the same shape DDR-1071 found in Group E, and it is the same tally
`CLAUDE.md`'s own DDR-1021 entry already records — *"Section 3C CLOSES at 6
shipped + 2 deferred + 0 buildable-and-unbuilt"* — which the table never
absorbed. The §CURRENT BUILD STATE prose and the Group F table have disagreed
about Section 3C since DDR-1021 was written.

---

## 2. The two remaining rows are a trap, and it is the FALSE POSITIVE DDR-1071 did not name

`ACTION_SEND_IPC` and `ACTION_RUN_EXPERIMENT` are genuinely **not** covered as
action types. And each has a gate in this tree whose **name matches the type**:

- **`smoke-sendipc`** (Makefile:3730, shard 7, **strict**) — `grep -n
  'ACTION_SEND_IPC\|SUBMIT_ACTION' user/ipctest.c` returns **one line, a
  comment**, and that comment says the type was deferred. The gate covers the
  DDR-1033 *door*: `SYS_IPC_SEND`/`SYS_IPC_RECV` (NSI 98/99), granted and
  refused arms, round trip. DDR-1033's own "NOT DONE: the AETHER action path
  does not yet CALL this" stands.
- **`smoke-runexp`** (Makefile:3748, shard 8, **strict**) — drives
  `QEMU_PROBES=exp`, i.e. DDR-1034's bounded stack machine through
  `SYS_RUN_EXPERIMENT`/`SYS_EXP_RESULT`. `grep -rn 'ACTION_RUN_EXPERIMENT'
  kernel/ user/` outside `aether.h` returns **one line: a header comment in
  `kernel/aether/experiment.h`**. Nothing submits the type.

**DDR-1071 §5 assessed the mechanical rule** — *a row whose named gate exists
and is registered must carry a marker* — **and refused it because it would
redden on `smoke-horizon`, which is correct in-progress state.** That is a false
negative: a red check someone investigates and then deletes.

**Here is the other direction, and it is the worse one.** Applied by name — and
`gate per type` invites exactly that — the rule **closes two rows whose adjacent
gates cover a different claim**. A false negative gets investigated; a false
positive is a row that silently stops being work. Both gates are strict tier and
both are green, so nothing anywhere would contradict the closure.

This is a second, independent reason DDR-1071's refusal was right, and it was
not available to DDR-1071. Carry both: the rule is unsafe in *both* directions,
and the distinguishing signal is semantic — *does the gate cover what the row's
prose asks?* — which nothing in the tree can read.

**The two rows stay OPEN.** No claim is made here about how hard either is;
DDR-1021 named the RUN_EXPERIMENT blocker and DDR-1034 removed part of it (the
subsystem now exists), which is a reason to re-assess, not a reason to close.

---

## 3. The four `CAP_*` wiring rows are a recorded refusal, not unbuilt work

`CAP_OCR` (1<<19), `CAP_EXEC` (1<<20), `CAP_SCENE` (1<<22), `CAP_NET_BROWSE`
(1<<23) each have a Group F row reading *"wire so \<agent\> is spawnable"*.
**DDR-982 §5.3 already decided this, and the decision is a withdrawal:**

> **Kept:** (A) the four bits in `cap.h`; (B) `tcb.agent_caps`, explicitly
> zeroed in `sched_create`.
> **Withdrawn pending a decision:** §2(C) enforcement and §2(D)
> `smoke-capagent`. Both require the action types to exist. *"A gate written now
> could only assert that a bit can be set and read back, which is a test of
> `uint32_t`, not of a capability system — the DDR-973 vacuity lesson applied to
> my own proposal."*

**Corroborated in the tree, not taken from the DDR:**

- `grep -rn agent_caps kernel/ user/` outside `sched.h` returns **exactly one
  line**: `sched.c:1122`, `t->agent_caps = 0;`. The field is written once and
  **read nowhere**.
- `aether.h:24-30` **deliberately omits** the four action types those bits gate,
  and says why in as many words: *"Six further 3C types are deliberately ABSENT
  until their subsystem exists … Declaring an enum value with no enforcement is
  worse than omitting it: an agent could submit one and the kernel would queue
  an action nothing implements."*

So there is nothing to submit and nothing to deny — the rows ask for enforcement
of a boundary whose far side the design deliberately does not admit.

**And three of the four are already in the PRE-APPROVED EXCEPTIONS table**
(*"capability bit defined, enforcement deferred — no subsystem path"*), so the
same item sits in this file **twice**, once as backlog work and once as a logged
deferral, with the work copy not saying so. The five *"Make PRAX / LUMYN /
AHNIS / IRIS / RUFLO spawnable"* rows each read *"After CAP_X wired"* and
inherit the same block; per DDR-1022 there is exactly **one** agent program
(`user/agent_base.c`), so "spawnable" there means a domain behaviour, not a
program.

### 3.1 A residual DDR-982 recorded and this DDR does not fix

DDR-982 §5.4 found the DDR-964 create-then-init race **in a second place**:
`sys_spawn_agent` calls `g_spawn_hook(task)`, which ends in `sched_unblock(ut)`,
and records the roster slot only afterwards — *"the agent is runnable before its
slot is known"*, so any per-slot authority minted afterwards lands after the
first run. Not fixed there and not fixed here: it is unobservable until there is
per-slot authority to race against, which is precisely what (C) withdrew.
Recorded again so it is not rediscovered a third time.

---

## 4. `CAP_EXEC` is HALF right, and is CORRECTED not closed — the `smoke-horizon` shape

Unlike the other three, `CAP_EXEC` **is** wired and enforced:

- `kernel/syscall/sys_experiment.c:35` — `t->exec_cap = cap_create(t->caps,
  RES_EXEC, EXEC_RES_ID, CAP_EXEC);`
- `:45` — `if (!cap_authorize(t->caps, t->exec_cap, RES_EXEC, EXEC_RES_ID,
  CAP_EXEC))`
- gated by `smoke-runexp` (shard 8, strict), whose deny arm is not decorative:
  `user/exptest.c:87` records that the refused process *"holds CAP_EXEC and lacks
  only is_exec"*, so the two checks are independently live — the DDR-1033 lesson
  applied.

But it is wired to **DDR-1034's stack machine**, not to the row's stated purpose
(`ACTION_EXEC_CODE` / PRAX), and `ACTION_EXEC_CODE` is itself a pre-approved
exception (*"needs sandboxed interpreter subsystem"*). So the row is corrected,
not closed — the mirror of `smoke-horizon` in DDR-1071.

**A `CLAUDE.md` claim is retired by this:** the DDR-1021 entry states *"CAP_EXEC
is a `#define` checked NOWHERE (zero matches in `kernel/*.c`, no `is_exec` on
struct tcb)"*. That was true when written; DDR-1034 built both. The entry is a
historical record and is left as written, but the Group F row that repeats its
conclusion is corrected.

---

## 5. Two more rows corrected, one of them a different staleness

**Per-agent live-metrics panel — half right.** `smoke-agentmetrics`
(Makefile:4150, shard 8, **strict**) and `smoke-agentpanel` (:4161, shard 6,
**strict**) both exist and are registered, and the row carries no marker. But
the row asks for *"CPU% sparkline, memory graph, action-rate histogram"* and
what ships is the metrics plumbing (pid + dispatches, post-mortem-stable per
DDR-735) plus DDR-737's **four activity pips** (`compositor.c:539`). Corrected,
not closed: the three named visualisations are not built.

**Agent `execve`-on-respawn — a blocker presented as live.** The row reads
*"needs FAT32 fix or SFS as agent root"*. The FAT32 defect was **REFUTED and
gated** — DDR-973, `smoke-fat32-multicluster`, shard 3, strict. One of the row's
two alternative blockers no longer exists and the row still names it.

That is a **different staleness from §1** and worth separating: §1 is work
presented as remaining, which understates progress; this is a **blocker
presented as live**, which can suppress work that is in fact unblocked. The
first wastes a re-derivation; the second stops the work happening at all.

---

## 6. A NEGATIVE finding: the registration half of this class is structurally impossible

The first sweep appeared to find `smoke-agentmetrics` **in the Makefile and not
in `gate_shards.txt`** — the inverse defect, a built gate CI never runs, which
would be worse than anything above. It was the grep that was wrong: the shard
file is `<shard>\t<target>\t<secs>\t<tier>`, so the target is **not** at line
start and `grep '^smoke-agentmetrics'` cannot match. Re-measured correctly it is
shard 8, strict.

Recorded because the correct conclusion is stronger than the retracted one:
`tools/ci/shard_check.sh:73-84` **asserts** that every `smoke-*` target in the
Makefile is either assigned to a shard or in the named EXCLUDE list, and fails
with the remedy printed otherwise. So *"a gate exists and CI never runs it"*
cannot survive `ci-shard-check`, which is a hygiene gate on every commit.

**The §7b class therefore survives only in the DOCUMENT** — which is exactly the
boundary DDR-1063 identified when it built `ci-docstate-check`, the first check
in this tree that reads a claim in a document rather than in the tree, and
exactly the boundary that check could not cross (it asserts an arithmetic
identity, not a semantic one).

---

## 7. What this changes, and what it does not

**Changes.** Group F now reads as: Section 3C **6 of 8 shipped and gated**, two
open with the reason each adjacent gate does not cover them; `CAP_EXEC` wired
and enforced (to a different consumer than the row named); the other three
`CAP_*` rows and their five dependent agent rows **refused pending an operator
decision**, DDR-982 §5.3; metrics plumbing shipped and gated with the three
named visualisations outstanding; and the agent-respawn row's FAT32 blocker
withdrawn. The genuinely open Group F work is the **domain agents** (F#66, F#67,
F#69-F#75), audit-ring SFS persistence, agent respawn, concurrency arbitration
and roster continuity — a materially smaller and differently-shaped list than
the table showed.

**The gate coverage did not change. Only the record of it did.**

**NOT CLAIMED.**

- **No code change.** Markdown only; `kernel.bin` untouched, size/headroom pair
  unaffected, gate count unchanged at 177.
- **No gate was re-run in this session.** What was measured is that these gates
  exist, are registered, are not excluded, and — for `smoke-sendipc`,
  `smoke-runexp`, `smoke-actionquery`, `smoke-actionhypo` and
  `smoke-actionspawn`, read in full — what they actually assert. Their green
  status comes from CI having run them.
- **`ACTION_SEND_IPC` and `ACTION_RUN_EXPERIMENT` stay OPEN** as action types.
  Neither is claimed easy and neither is re-assessed here.
- **No operator decision is made** on DDR-982 (C)/(D), and none is implied. The
  four `CAP_*` rows are re-labelled from *unbuilt* to *refused pending a
  decision*, which is a different thing from being resolved.
- **DDR-982 §5.4's create-then-init race is NOT fixed** — recorded only.
- **No open issue moves.** OPEN-1, OPEN-2, OPEN-12 and OPEN-13 are untouched.
- **No checker is built**, for the reason in §2 — and this DDR supplies the
  second half of the argument against one, not a case for it.
