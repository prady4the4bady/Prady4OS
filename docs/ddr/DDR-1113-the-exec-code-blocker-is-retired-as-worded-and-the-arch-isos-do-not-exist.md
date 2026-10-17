# DDR-1113 — `ACTION_EXEC_CODE`'s blocker is retired AS WORDED, the arch ISOs do not exist, and Section 3C closed two DDRs ago

**Status:** ASSESSMENT + CORRECTION. Docs-only: **no code change, no gate, NO
DEFECT FOUND AND NONE ALLEGED.**
**Date:** 2026-09-13
**Scope:** `docs/PRE_LAUNCH_CHECKLIST.md` §5.1, §5.1b, §5.2 — **the last three
unaudited blocks.** §1, §3, §4, §5.3, §5.4 and §6 were audited in DDR-1086,
DDR-1107, DDR-1110 and DDR-1111. **This completes the audit of the checklist.**

Written on a green tip (`ff80e56`, 32/32, DDR-1112 §10), so no CI wait is being
filled and no kernel change is stacked under a running suite.

---

## §0 — What this found

Three corrections, in descending order of what they cost a reader:

1. **§5.1 `ACTION_EXEC_CODE`: the stated blocker is FALSIFIED as worded, and the
   real blocker is somewhere else.** Dangerous direction (§1).
2. **§5.1 the multi-arch rows describe an ISO that does not exist, via a
   mechanism that is not implemented.** (§2)
3. **§5.2 Section 3C says 6 shipped + 2 deferred; it is 8 of 8**, and both of
   the "traps" it warns about are resolved. Safe direction, but the trap
   paragraph actively misinstructs (§3).

**§5.1b is accurate in every particular** and is stated as such in §4, because an
audit that only reports errors is not an audit.

---

## §1 — `ACTION_EXEC_CODE`: a sandboxed interpreter EXISTS, and that is not good news

§5.1's table row reads:

| `ACTION_EXEC_CODE` | needs a sandboxed interpreter subsystem |

**A sandboxed interpreter subsystem has existed since DDR-1034 and is CI-gated.**
`kernel/aether/experiment.c` is a bounded integer stack machine the kernel runs
on an agent's behalf, driven by `smoke-runexp` (shard 8, strict), with `CAP_EXEC`
minted and `cap_authorize`d at `sys_experiment.c:35`/`:45`. So the sentence as
written is no longer true.

**And it must NOT be read as retiring the row.** `experiment.h`'s header states
the property that makes that machine safe, and it is the exact property
`ACTION_EXEC_CODE` would have to destroy:

> **THE SECURITY ARGUMENT IS THE INSTRUCTION SET, NOT A GUARD.** There is no
> LOAD, no STORE and no addressing mode, so "no memory access outside its own
> stack" is a property of **what can be encoded** — it cannot be lost by
> deleting a check.

Measured, not inferred: the opcode set is `HALT / PUSH / ADD / SUB / MUL / DUP /
DROP / SWAP / JNZ` and nothing else, bounded at `EXP_MAX_STEPS` retired
instructions on a 32-slot stack, with `DIV` **absent rather than guarded**
because a `#DE` in ring 0 is fatal. An approved experiment computes an integer
and **can touch nothing**.

`ACTION_EXEC_CODE` / PRAX (`shell_agent`) means executing agent-authored code
that *does* something. **That is precisely what this machine cannot express**, by
construction and on purpose. So the honest blocker is not *"no sandboxed
interpreter exists"* — it is:

> **The shipped interpreter is deliberately incapable of the effects
> `ACTION_EXEC_CODE` needs, and giving it those effects means replacing an
> encodability property with a checkable guard — on an agent-facing path.**

Adding LOAD/STORE turns *"cannot be lost by deleting a check"* into *"a check
that can be deleted"*. That is the trade `experiment.h` refuses in its own
header, and it is not a trade a session should make while closing a table row.

**THIS RELOCATES THE BLOCKER AND DOES NOT SHRINK IT** — the DDR-1104 shape (the
TLS row's named library was the wrong obstacle; the trust anchor was the real
one) and the DDR-1110 §3 shape (F#73's two named blockers were gone and the real
one was never stated). **DDR-1110's rule applies verbatim and is why this section
is worded the way it is: a correction that removes a wrong blocker without naming
the right one is worse than the blocker it removed.**

**Why the dangerous direction.** A session reading *"needs a sandboxed
interpreter subsystem"*, then noticing `experiment.c` exists, is gated, and even
mints the row's own `CAP_EXEC`, would reasonably conclude the exception is spent
and go wire `ACTION_EXEC_CODE` to it. The shortest path from there is widening
the instruction set. The stale sentence would at least have stopped them.

**The row STAYS DEFERRED**, on the relocated ground. `ACTION_EXEC_CODE` remains
absent from the action enum — measured, its only occurrence anywhere in `kernel/`
or `user/` is a **comment** at `cap.h:62` — which is `aether.h`'s own stated
policy (*"declaring an enum value with no enforcement is worse than omitting
it"*) working exactly as intended. **Nothing is built, nothing is unblocked, and
no operator decision is taken.**

**DDR-1034 IS NOT CRITICISED.** Its work is real, its security argument is sound,
and it had no obligation to this row. This is the DDR-1084 §1 family again: the
session that retires a blocker is working on the *subsystem*, not on the *row*.

---

## §2 — The multi-arch rows describe an artefact that does not exist

§5.1's table says:

| Apple Silicon / m1n1 | post-1.0 — **the aarch64 ISO uses the U-Boot path** |

and the prose beneath it says *"**The ISOs for those architectures** package a
kernel that boots and does not run userspace."*

**Measured, in the Makefile and tree-wide:**

- **There is exactly ONE `iso:` target and it is x86.** `Makefile:1160` is
  `iso: $(ISO_HD) esp-image`, consumed by `smoke-iso-x86` (`:1170`) and
  `smoke-iso-userspace` (`:1200`). **There is no `iso-aarch64`, no
  `iso-riscv64`, and no aarch64 or riscv64 ISO of any kind.**
- **U-Boot does not exist in this repository outside planning prose.** A
  tree-wide grep over `Makefile`, `*.c`, `*.S`, `*.ld` returns **nothing**; the
  only hits are four lines of `docs/PRADYOS_MASTER_PLAN.md`, and that document's
  own table marks them **`⬜` NOT DONE** — *"aarch64 | ✅ CI | ⬜ EFI/U-Boot"*.

So the row does not merely overstate: **it explains how a non-existent artefact
works, by naming a mechanism that is itself unimplemented and recorded as
unimplemented in the document it came from.**

**The deferral is CORRECT and is not disturbed.** Apple Silicon / m1n1 is rightly
post-1.0. What is wrong is the *reason*: a session reading "we already have the
U-Boot path on aarch64" is reasoning from something that is not there, and would
mis-scope any follow-up.

**The prose's CONCLUSION survives and is strengthened, not weakened.** §5.1 ends
by saying that if the release is described as "multi-architecture" that wording
must carry a boot-only qualifier, and that this belongs with §1.2. **True — and
the honest premise is stronger than the one stated:** there is no aarch64 or
riscv64 ISO *at all*, so the claim needing a qualifier is not "these ISOs are
boot-only" but "these ISOs do not exist". What *is* shipped and green is the
**boot stub in its own CI job** — `smoke-aarch64` / `smoke-riscv64`, `arch-bootstrap`,
both `success` on this tip — which is ADR-034's scope correctly implemented and
is not an ISO.

**This is DDR-1081 §1.4's finding arriving in a second document.** That DDR
corrected CLAUDE.md's Group H rows and the WHAT-DONE-MEANS boxes, and did not
open the checklist — **DDR-1086 §1's shape exactly: opens ONE of the files and
not the others.** One further instance of a family already named; **no rate is
claimed.**

---

## §3 — Section 3C closed at 8 of 8 two DDRs ago

§5.2 states:

> **Section 3C action types close at 6 shipped + 2 deferred + 0
> buildable-and-unbuilt** … `SEND_IPC` — see §3 and §4.1. `RUN_EXPERIMENT` — see §3.

and then warns, at length, that the two deferred types are **a trap**, because
`smoke-sendipc` and `smoke-runexp` each have a *name* matching the type and a
*claim* that is something else.

**Both halves are now false. Section 3C closes at 8 of 8** — DDR-1083 wired
`ACTION_RUN_EXPERIMENT`, DDR-1084 wired `ACTION_SEND_IPC`, and each explicitly
resolved the corresponding trap.

**Verified in the tree rather than taken from CLAUDE.md** (DDR-1007's discipline):

| type | probe | submits it | gate sentinel required |
|---|---|---|---|
| `ACTION_RUN_EXPERIMENT` (11) | `user/actionexptest.c` (8,810 B) | `:153` `nsi(SYS_SUBMIT_ACTION, ACTION_RUN_EXPERIMENT, …)` | `PRADYOS_EXPACT_A st=2 ran=1 rc=0 v=42` + `…_B` + `…_OK` (`Makefile:3910`) |
| `ACTION_SEND_IPC` (7) | `user/actionipctest.c` (8,783 B) | `:136` `nsi(SYS_SUBMIT_ACTION, ACTION_SEND_IPC, …)` | `PRADYOS_IPCACT_A st=2 sent=1 rc=0 back=0x00000000A71C0001` + `…_B` + `…_OK` (`Makefile:3892`) |

Both probes hand-copy the type constant (`:72` and `:68`), so the gates now cover
**the door and the action type** on the same boot, which is exactly what the trap
paragraph said they did not.

**Direction: SAFE — it understates progress.** But the trap paragraph is worse
than a stale tally, and that distinction is the reason this is written down
rather than silently edited: a bare `6 of 8` merely under-reports, while
*"read this before closing either"* **instructs** a future session that those
gates' claims do not cover the types. They would open the probes, find the
submissions, and re-derive DDR-1083/1084's work to get back to where the tree
already is.

**And the pattern is sharp enough to state plainly.** DDR-1084 §1 is the DDR that
**named** this failure — *"a DDR that retires a blocker does not, by default,
revisit the row the blocker was holding"* — named it from two instances, and
declined to build a checker because the signal is semantic. **It then updated
CLAUDE.md's Group F rows correctly and left this row in a different file.** Not
forgotten in the abstract: the pattern's own author missed its own instance, in
the file one directory across. DDR-1110 §2 found the identical thing inside
DDR-1083's text. **This is a further instance of a family already named, not a
new pattern, and no rate is claimed.**

---

## §4 — What is accurate, stated because an audit that only reports errors is not an audit

**§5.1b is correct in every particular**, re-measured rather than assumed:

- **Quantum hardware WITHDRAWN, PQC mandatory** — and the PQC half is not a
  promise, it shipped: `kernel/crypto/` holds `keccak.c/.h`, `keccak_kat.h`,
  `mldsa.c/.h` and **three** KAT headers (`mldsa_kat.h`, `mldsa_sig_kat.h`,
  `mldsa_ver_kat.h`), i.e. keyGen + sign + verify (DDR-1052/1054/1057/1058).
- **§5.1b.1's own headroom note is correctly frozen and correctly annotated** —
  it states the PRE-work figures, says so, and carries a block quote giving the
  live ones. That is the right handling of a dated assessment (the DDR-1081 §5 /
  DDR-1111 rule: date-stamp the status, leave the argument text alone), and it
  is the shape §1.5 had to be given retrospectively.

**§5.1's other rows check out:** Intel HDA is absent from `kernel/` entirely;
wlroots/Wayland appear nowhere except `user/compositor.c:7` saying *"NOT
wlroots/Wayland"*, which is the row's claim in the source.

**§5.1's NVMe row is RIGHT WHERE CLAUDE.md's WORK ROW WAS WRONG**, and that is
worth recording because it inverts the usual direction. §5.1 gives only the
**exception ground** — *"poll-mode is sufficient for the ISO (DDR-774a/b/c)"* —
which DDR-1103 §1 established is the **surviving** ground, as against the *"on
hold until B#3 SMP is stable"* blocker in CLAUDE.md's Group B row, B#3 being
closed (`ROOT-CAUSED AND FIXED — DDR-981`). The checklist stated the durable
reason and the backlog table stated the spent one.

**§5.2's structural fact and its F#74 analysis both still hold.** There is still
exactly one agent program (`user/agent_base.c`, DDR-1022), so "11 unbuilt agents"
is still 11 behaviours; and `agent_caps` is still **written once and read
nowhere** — grep outside `sched.h` returns exactly `sched.c:1156`,
`t->agent_caps = 0;`. (§5.2 cites `sched.c:1122`; the line has **drifted to
:1156** — the DDR-1073 §5 / DDR-1094 lesson that a row citing a line number has
an expiry date nothing in the tree can check. The **fact is unchanged**, so this
is noted, not treated as a finding.) §5.2's `CAP_EXEC` paragraph is correct and
is the same measurement §1 above starts from.

---

## §5 — No checker, and the reason is the one already measured four times

The available mechanical signal is *"a checklist row's stated blocker is no
longer true"*, which is **semantic**. §1 is the proof that it cannot be
mechanised here: a grep would see that a sandboxed interpreter exists and
conclude the row is closeable, which is the **opposite** of the correct answer —
the blocker is real and is somewhere else. The same wall DDR-1071 §5,
DDR-1072 §2, DDR-1081 §3, DDR-1086 §4 and DDR-1107 §3 each measured and each
refused a checker for. `ci-docstate-check` remains the shape that works,
because it asserts an **arithmetic identity**.

**The cheap substitute, unchanged and applied here:** a correction belongs in the
row it corrects (DDR-1110 §4), and a DDR that retires a blocker names the rows
that blocker was holding (DDR-1084 §1, sharpened by DDR-1107 §3 to attach the
obligation to **shipping a named remedy**).

---

## §6 — NOT CLAIMED

- **NO code change, NO gate, NO new sentinel.** `kernel.bin` **not rebuilt**, so
  the size/headroom pair and `ci-docstate-check` are unaffected;
  `GLOBAL_FORBIDDEN` **77**; **179 gates**; probe ELF count unchanged (and see
  DDR-1112 §10.2 before quoting an absolute figure for it).
- **NO defect is found in any code and none is alleged.** `experiment.c`,
  `sys_experiment.c`, both action probes, both gates, the arch boot stubs, the
  x86 `iso` target and `agent_base.c` are all correct for what they were built to
  do. **What is corrected is the ROWS.**
- **`ACTION_EXEC_CODE` IS NOT UNBLOCKED** and stays a pre-approved exception. No
  interpreter is widened, no opcode is added, `experiment.h`'s instruction set is
  untouched, and whether the row should ever be built is an operator decision
  (the DDR-793/982 class) — **recorded, not taken.**
- **NO ISO is built for aarch64 or riscv64**, no packaging is designed, ADR-034's
  boot-only scope is **not** revisited, and §1.2's release-wording decision is
  **not** made — it is the operator's and stays in Section 1.
- **Section 3C's 8-of-8 is CITED, not re-proved here** — DDR-1083 and DDR-1084
  carry the arms and the mutants. What this DDR measured is that the probes
  submit the types and the gates require the sentinels.
- **DDR-1034, DDR-1081, DDR-1083 and DDR-1084 are NOT criticised.** Each did real
  work and stated its own residual accurately in its own text; the defect is in
  the RECORD, which is the DDR-1084 §1 failure it named itself.
- **NO rate and no new pattern claimed** — §2 and §3 are further instances of
  families already named from two or more.
- **NO checker built**; DDR-1086 §4's refusal stands.
- **NO gate was run for this DDR.** What was measured: the `iso:`/`smoke-iso-*`
  targets in the Makefile, a tree-wide U-Boot grep, `PRADYOS_MASTER_PLAN.md`'s
  own `⬜` markers, `experiment.h` read in full, a tree-wide `ACTION_EXEC_CODE`
  grep, `ACTION_RUN_EXPERIMENT` / `ACTION_SEND_IPC` greps outside `aether.h`,
  both probes' submit call sites, both gates' `EXTRA_SENTINEL` lines,
  `ls kernel/crypto/`, an `agent_caps` grep, and HDA / wlroots greps.
- **NO open issue moves** (OPEN-1/2/12/13 untouched); not an apfreeze, not OPEN-2.
- **NO release action is taken or proposed.** `v1.0.0` stays untagged, the
  promotion stays unstarted, and the three-greens count is unaffected by a
  docs-only commit.
