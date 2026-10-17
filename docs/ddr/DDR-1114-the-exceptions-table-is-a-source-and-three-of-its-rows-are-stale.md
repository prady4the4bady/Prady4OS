# DDR-1114 — THE EXCEPTIONS TABLE IS A SOURCE, NOT A STATUS LIST, AND THREE OF ITS ROWS ARE STALE

**Date:** 2026-09-13
**Type:** assessment + correction. **Docs-only. No code change. No gate. NO DEFECT
FOUND IN ANY CODE AND NONE ALLEGED.**
**Written in:** the CI-wait window on `83ac6e5`, with both suites' shards queued and
the build job's digest already confirmed byte-identical (§0.1).

---

## §0 — WHAT THIS IS

`CLAUDE.md` §PRE-APPROVED EXCEPTIONS is seventeen rows. Every backlog table in that
file has now been audited (E=DDR-1071, F=1072, A/B=1073, G=1075, H=1081, D=1101/1102,
B=1103, C=1104) and `docs/PRE_LAUNCH_CHECKLIST.md` was completed one commit ago by
DDR-1113. **This table has never been read row by row.** It is the last one.

Three of its rows are false. **Two of the three were falsified by DDR-1113 — the
commit immediately before this one — and the third by DDR-1103.** §3 is about that,
and it is not a comfortable finding to write, because DDR-1113 §3 *named this exact
pattern* and then produced an instance of it.

### §0.1 — the CI verdict this was written against

Read at 05:55 UTC from build job `103678016077` (suite `34740011216`), not inferred:

```
95493b96c7d13f30d83bb116d6d9157922081db827ee30d61ca971f7d6674d94  kernel.bin
kernel: build/kernel.bin (1319306 bytes)
probe-rodata-check: OK — 77 ELFs, none carry a writable allocated section
```

Byte-identical to `ff80e56`'s digest and to the local build, so **four docs-only
commits did not move the binary** — the proof form DDR-1009/1074/1090 rest on holds.
The `77 ELFs` line is a **second, independent observation** of DDR-1112 §10.2 (77 in
CI, 79 locally, the gap being exactly ADR-034's two cross-arch kernels, which CI
builds in the separate `arch-bootstrap` job). §10.2 was written from one observation
and said so. **This is a confirmation, not a new finding, and is reported at that
weight.** Shards were still in flight when this was written; nothing here depends on
their verdict.

---

## §1 — THE TABLE IS A SOURCE, AND THE PROPAGATION IS MEASURED

The distinction that makes this worth its own record is in the table's **own header**,
one line above the first row:

> *"For each: add a one-line entry in `docs/BUILD_TRACKER.md` as `[DEFERRED: reason]`."*

So this is not a status list that a reader consults. **It is a source that its own
instruction says to COPY**, and the copies exist. Measured, not assumed:

| CLAUDE.md row | propagated to |
|---|---|
| `"deferred post-1.0 — aarch64 ISO uses U-Boot path"` | `BUILD_TRACKER.md:1055`, verbatim |
| `"needs sandboxed interpreter subsystem"` | `BUILD_TRACKER.md:1060`, verbatim |
| `"poll-mode sufficient for ISO; … deferred until B#3 SMP stable"` | `BUILD_TRACKER.md:1068`, verbatim |

`ACTION_EXEC_CODE` has a **fourth** copy, elaborated rather than quoted, at
`BUILD_TRACKER.md:289`: *"needs a sandboxed interpreter — a subsystem, not an action"*.

**A stale row here is therefore not merely stale — it is instructed to be replicated,
and it has been.** That is a different and worse property than the staleness shapes
already on record (§7b shipped-but-unmarked, §7c gate-for-unbuilt-work, the
`smoke-horizon` half-done shape, DDR-1084 §1's stale blocker): each of those misleads
a reader of one row. **This one manufactures more rows.**

---

## §2 — THREE ROWS, THREE DIFFERENT STALENESS SHAPES

### §2.1 — `ACTION_EXEC_CODE`: *"needs sandboxed interpreter subsystem"* — FALSE

One has existed since **DDR-1034**. Measured in the tree, not taken from DDR-1113:

* `kernel/aether/experiment.c` (4,726 B) and `kernel/aether/experiment.h` (3,097 B) ship.
* It is **CI-gated** — `smoke-runexp` at `Makefile:3907`.
* It **mints this row's own capability**: `sys_experiment.c:35`
  `t->exec_cap = cap_create(t->caps, RES_EXEC, EXEC_RES_ID, CAP_EXEC);` and `:45`
  `cap_authorize(…, CAP_EXEC)`.

**AND THE ROW STAYS DEFERRED.** DDR-1113 §1 established why and it is not repeated
here beyond the one sentence that has to travel with the correction: `experiment.h`'s
safety is **the instruction set, not a guard** — no LOAD, no STORE, no addressing
mode — so *"no memory access outside its own stack"* is a property of **what can be
encoded** and cannot be lost by deleting a check. An approved experiment computes an
integer and touches nothing; `ACTION_EXEC_CODE` / PRAX means code that **does**
something, which that machine cannot express **by construction**. The real blocker is
that giving it those effects **replaces an encodability property with a checkable
guard on an agent-facing path**.

**THE DIRECTION IS DANGEROUS AND THE REMEDY IS NOT DELETION.** A session that greps,
finds the interpreter, finds it gated, finds it minting `CAP_EXEC`, and concludes the
exception is spent would widen the opcode set — and **the stale sentence would at
least have stopped them** (DDR-1110's rule). So the row is **corrected in place with
the blocker relocated**, exactly as DDR-1113 did in the checklist, and never removed.

### §2.2 — Apple Silicon / m1n1: *"aarch64 ISO uses U-Boot path"* — FALSE TWICE OVER

Re-measured independently of DDR-1113 (DDR-1007's discipline — do not inherit):

* **U-Boot exists nowhere in the build or the source.** `grep -rniE 'u-boot|uboot'`
  over `Makefile` and every `.c/.h/.S/.asm/.ld` returns **nothing**. The only hits
  tree-wide are Markdown: this file, `SESSION_HANDOFF.md`, `BUILD_TRACKER.md`,
  `PRADYOS_MASTER_PLAN.md`, `PRE_LAUNCH_CHECKLIST.md`, and three DDRs.
* **There is exactly one `iso:` target and it is x86** — `Makefile:1160`, consumed by
  `smoke-iso-x86` (`:1170`) and `smoke-iso-userspace` (`:1200`). There is **no
  `iso-aarch64` and no `iso-riscv64`**.
* `m1n1` likewise appears in Markdown only.

So the row does not merely overstate: **it explains how a non-existent artefact works,
via a mechanism that appears nowhere in the build.** The **deferral is correct** and is
not disturbed; what is wrong is the **reason**.

### §2.3 — NVMe completion IRQ: *"deferred until B#3 SMP stable"* — HALF STALE

**B#3 is closed.** `CLAUDE.md:22` carries it in the operator directive itself:
*"~~B#3 virtio-blk SMP stall must be fixed before ISO.~~ **DONE — DDR-981.**"*, and
§OPEN ISSUES records the mechanism (`yield()` spun with `RFLAGS.IF` clear; 20/20 at
`-smp 4`).

This row is **the least wrong of the three and is the one worth reading carefully**,
because it states **both** reasons: *"poll-mode sufficient for ISO"* (durable, still
true) **and** *"until B#3 SMP stable"* (spent). DDR-1103 §1 established that the
verdict survives on the first and not the second, and DDR-1113 §4 noted that
`PRE_LAUNCH_CHECKLIST` §5.1's NVMe row is **right** precisely because it gives only
the durable ground. So this row leads with the surviving reason and then appends the
retired one — a session reading only the tail concludes it is now free to build.

DDR-1103 §1(a) also recorded that this item appears in `CLAUDE.md` **three times**
(exceptions, §DEFERRED, and the Group B work row) and corrected the **work row**.
**This is the exceptions copy**, still carrying the spent blocker.

### §2.4 — and the §DEFERRED list inherits the same false premise

`CLAUDE.md:981` reads *"`arch/aarch64` / `arch/riscv64` full ports (boot-only scope,
**ISOs use that**)"*. Same presupposition as §2.2: there are no such ISOs. The two
`arch/*` exception rows (`:865`, `:866`) say *"ISO uses boot-only kernel"* — which is
the **relatively** accurate wording (see §5) and still presupposes an artefact that is
not built.

---

## §3 — THE PATTERN, AND IT IS THIS AUTHOR'S OWN, ONE COMMIT AGO

DDR-1113 §3 wrote, about DDR-1084:

> *"THE PATTERN'S OWN AUTHOR MISSING ITS OWN INSTANCE ONE DIRECTORY ACROSS."*

**DDR-1113 then did it.** It corrected `PRE_LAUNCH_CHECKLIST` §5.1's copies of the
U-Boot claim and the interpreter claim — and left the identical claims standing in
`CLAUDE.md`'s exception rows and in `BUILD_TRACKER`'s deferral list. This is the
**fourth** instance of the DDR-1084 §1 family and it takes the **DDR-1086 §1 shape**
(opens ONE file and not the others), which DDR-1113 cited **by name** while producing
it.

**And it is sharper than any previous instance, because of a fact inside one file.**
DDR-1113's correcting text sits at `BUILD_TRACKER.md:4795`; the stale row it corrects
sits at `BUILD_TRACKER.md:1060` — **the same file, ~3,700 lines apart.** That is
DDR-1110 §1's finding — *"a correction that supersedes a row from a DIFFERENT document
does not change what a reader of that row sees"* — reproduced **inside a single
document**, where "different document" is not even available as an excuse.

**NO RATE IS CLAIMED AND NO NEW PATTERN IS CLAIMED.** This is one further instance of
a family already named from four, and the honest reading of the family is now
structural rather than incidental: **the mechanism is not forgetting, it is that a
DDR's working set is the file it is arguing about**, and the rows it falsifies live
wherever they happen to live. DDR-1111 §3 reached the same conclusion from a different
direction ("nothing was done wrong at either step").

---

## §4 — WHAT IS ACCURATE, STATED BECAUSE AN AUDIT THAT ONLY REPORTS ERRORS IS NOT AN AUDIT

**Fourteen of seventeen rows are correct.** Specifically checked rather than assumed:

* **Wayland/wlroots** — *"superseded by shipped custom C framebuffer compositor"* is
  right; DDR-1113 §4 measured that the strings appear nowhere but `compositor.c:7`
  saying *"NOT wlroots/Wayland"*.
* **Intel HDA** — absent from `kernel/` entirely.
* **`CAP_OCR` / `CAP_SCENE`** — *"capability bit defined, enforcement deferred — no
  subsystem path"* is exactly DDR-982 §5.3's recorded refusal, corroborated in the
  tree by DDR-1072 §3 (`agent_caps` written once, read nowhere).
* **SFS block reclamation on-disk** — accurate, and DDR-1073 §3 confirmed the
  in-memory half shipped (DDR-762-v2) while the on-disk free tree is genuinely a
  logged deferral.
* **Cloud bridge / `ACTION_BROWSE_WEB`** — DDR-793's security-posture deferral,
  re-confirmed by DDR-1110 §3 when it relocated F#73's blocker onto it.
* **Rust rewrite**, **CMake/Makefile hybrid (DDR-843)**, and the four post-L7
  `ACTION_*` rows — no subject exists for any of them; nothing to correct.

**The three `ACTION_*` post-L7 rows are also a useful negative:** they name hardware
and subsystems that genuinely do not exist, so the same mechanical signal that fires
on §2.1 ("the row says a subsystem is needed") is **correct** on those. That is §7's
argument in miniature.

---

## §5 — A CORRECTION TO DDR-1081's READING, WHICH IS NOT A CRITICISM OF IT

DDR-1081 §1.4 cited this table as the **accurate** counterpart to the Group H work
row: *"this file already states the scope correctly elsewhere — §PRE-APPROVED
EXCEPTIONS carries `arch/aarch64 full port — boot-only scope per ADR-034`"*, and
concluded the **work** copy was the misleading one.

**That comparison was right and it is narrower than it reads.** DDR-1081's point was
about which of two rows misleads *more* — and the exception row does say "boot-only
kernel" where the work row implied a full OS, so it is the better of the two. What
DDR-1113 §2 then established is that **both share a false premise**: "ISO uses
boot-only kernel" presupposes an ISO, and no aarch64 or riscv64 ISO is built. So
DDR-1081's **relative** judgment survives intact; only its implication that the
exception row needs no correction does not.

Recorded because a future session reading DDR-1081 alone would take this table's
`arch/*` rows as already-audited, which is exactly how a row goes uninspected for
thirty DDRs.

---

## §6 — THE EDITS, ALL AT THE SITES

Per DDR-1110 §4 — **a correction belongs in the row it corrects** — and per §1, which
makes that rule load-bearing here rather than stylistic: a correction filed anywhere
but the row leaves the *source* intact and the copies keep being made from it.

**`CLAUDE.md` §PRE-APPROVED EXCEPTIONS:**
1. `:858` Apple Silicon / m1n1 — reason corrected; **deferral unchanged**.
2. `:863` `ACTION_EXEC_CODE` — blocker **relocated, not removed**; **deferral
   unchanged**; carries the explicit "do not widen the opcode set" warning, because
   §2.1's direction is dangerous.
3. `:865`/`:866` `arch/aarch64` / `arch/riscv64` — the ISO presupposition named; the
   ADR-034 boot-only scope, which is correct, left standing.
4. `:871` NVMe completion IRQ — the spent B#3 clause marked spent, the durable
   poll-mode ground kept as the operative one.

**`CLAUDE.md` §DEFERRED:** `:981`'s *"ISOs use that"* corrected.

**`docs/BUILD_TRACKER.md`:** the three propagated copies at `:1055`, `:1060`, `:1068`,
and the elaborated fourth at `:289` — because §1 is the whole finding, and correcting
the source while leaving the copies is the error this DDR is about.

**EXPLICITLY NOT DONE:** no row is **closed**, no deferral is lifted, and no reason is
deleted. Every edit keeps the original wording visible and adds the correction beside
it, the DDR-1110 §4 / DDR-1111 shape — because the record of what was believed and
when is what NON-NEGOTIABLE 5 exists to protect, and DDR-1081 §5 already measured what
the obvious tidy-up destroys.

---

## §7 — NO CHECKER, AND §4 IS THE PROOF IT CANNOT BE MECHANISED

The available mechanical signal is *"a row names a subsystem as missing and the
subsystem exists in the tree"*. §2.1 would fire on it — **and so would the four post-L7
`ACTION_*` rows fire on the inverse, correctly, because for those the subsystem really
is absent.** More decisively: a grep that found `experiment.c` would conclude the
`ACTION_EXEC_CODE` row is **closeable**, which is **the opposite of the correct
answer** (§2.1). A checker here would not merely be noisy; **it would be confidently
wrong in the dangerous direction.**

That is the same wall DDR-1071 §5, DDR-1072 §2, DDR-1081 §3, DDR-1086 §4, DDR-1107 §3
and DDR-1113 §5 each measured and each refused a checker for. `ci-docstate-check`
remains the shape that works because it asserts an **arithmetic identity**.

**THE CHEAP SUBSTITUTE, SHARPENED FROM DDR-1107 §3 AND DDR-1111 BY §1's PROPERTY:**
those attached the obligation to *shipping a named remedy* and to *citing a section as
your own design*. §1 adds a third and more mechanical trigger, because it needs no
judgment at all: **a document that instructs its rows be copied elsewhere owes a
correction to every copy, and the copies are enumerable by grep.** The three
`BUILD_TRACKER` lines were found in one command.

---

## §8 — NOT CLAIMED

* **NO code change, NO gate, NO new sentinel.** `kernel.bin` **not rebuilt**, so the
  size/headroom pair and `ci-docstate-check` are unaffected. `GLOBAL_FORBIDDEN` **77**,
  **179 gates**.
* **NO defect found in any code and none alleged.** `experiment.c`, `sys_experiment.c`,
  `smoke-runexp`, the ADR-034 boot stubs, the x86 `iso:` target and `nvme.c` are all
  correct for what they were built to do. **What is corrected is the ROWS.**
* **`ACTION_EXEC_CODE` IS NOT UNBLOCKED** and stays a pre-approved exception. **NO
  interpreter is widened, NO opcode added, `experiment.h`'s instruction set
  UNTOUCHED.** Whether that row should ever be built is an **operator decision,
  recorded and not taken.**
* **NO ISO is built** for aarch64 or riscv64, ADR-034's boot-only scope is **not
  revisited**, and §1.2's release-wording decision is **not made**.
* **NVMe: nothing is unblocked.** No completion IRQ is built, DDR-774a/b/c are not
  revisited, and the deferral stands on the poll-mode ground.
* **DDR-1113 IS NOT WITHDRAWN OR CRITICISED.** Its three findings stand and its
  corrections to the checklist are right; §3 records that it did not reach the copies,
  which is a statement about the *record*, and §3's own conclusion is that the
  mechanism is structural.
* **DDR-1081 IS NOT CRITICISED** — §5 corrects an implication, and its relative
  judgment survives.
* **NO rate and no new pattern claimed** — one further instance of a family already
  named from four.
* **NO gate was run.** What was measured: `grep -rniE 'u-boot|uboot'` and `'m1n1'`
  over `Makefile` and all `.c/.h/.S/.asm/.ld` (empty) and tree-wide (Markdown only);
  every `iso`-bearing Makefile target enumerated; `ls` on `experiment.c`/`.h`;
  `smoke-runexp`'s recipe line; `CAP_EXEC`'s three occurrences in `sys_experiment.c`;
  `CLAUDE.md:22`'s B#3 closure; and the four `BUILD_TRACKER` propagation sites.
  The CI facts in §0.1 were **read from the build job log**, not inferred.
* **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.
* **NO release action taken or proposed**; `v1.0.0` stays untagged and the hold stands.
