# PRADYOS — Claude Code session rules

PRADYOS Sovereign Edition is a from-scratch, bare-metal, AI-native OS for x86_64.
This file is kept short on purpose; it loads in every session. The full former
CLAUDE.md (invariants, history, old backlog tables) is preserved at
`docs/archive/CLAUDE-2026-10-17.md`. Read it **only when a task needs it**
(index: `docs/archive/README.md`).

## First actions, every session

1. Call `graph_session_primer` (pradyos-graph MCP; CLI: `node tools/graph_mcp/server.js primer`).
2. Read `docs/VISION.md` (the operator's vision; it wins on conflicts).
3. Read `SESSION_HANDOFF.md` (current state only).
4. Read issue #45 in full, including every owner comment (`prady4the4bady`). The **newest** owner comment wins over anything older, including this file.
5. Continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`.

## Authority and work order

- Issue #45 owner comments are the standing authorization (6177490081, 6177525415, 6195615129, 6199670683, 6199872798, 6200777349, 6201120568, 6201467383, 6201619419, 6229808666, 6233854639, 6256077528, 6256365961, 6282431738, 6301435736).
- Work order is `docs/V1_MASTER_CHECKLIST.md`. C11.1 (Rust and build system) stays **last** in build order.
- Before each step and before each merge, re-check #45 for new owner comments and follow them first.
- Never idle while an unblocked item exists. Stack the next item while CI runs.
- If something blocks you, post one comment on #45 with the exact blocker, then continue with the next unblocked item. Never work around a permission refusal.
- No `v1.0.0` tag and no promotion to `main` until blocks A to K are done and the release order in 6177525415 is satisfied.

## Merge rule (6229808666 item 1; supersedes the old three-greens rule)

- Merge a PR when **one** full `pradyos-ci` run is green on its **exact head SHA**. Do not start extra runs on a SHA that already has a run.
- Use a **merge commit**, not squash (6177490081).
- A new gate merges only after its mutants have been run in CI and **shown red**. A gate never seen red proves nothing.
- After every merge:
  - tick `docs/V1_MASTER_CHECKLIST.md` and record the merge SHA;
  - update `SESSION_HANDOFF.md` (short form);
  - post one short status comment on #45 (merged, open, approximate tokens used, graph_* calls made).
- The gh GraphQL API is blocked. Use REST:
  - mark ready: `POST repos/{o}/{r}/pulls/{n}/ccr/ready_for_review`
  - merge: `PUT .../pulls/{n}/merge` with `merge_method=merge` and `sha=`

## Non-negotiables

1. **Root-cause fixes only.** No fix without a named mechanism from a real failing artefact.
2. **Design before code.** Write the DDR, then implement.
3. **Every new gate gets a mutation check.** Mutants must land on different arms. Check every arm for vacuity *before* writing it: would it pass on the unfixed tree?
4. **Zero warnings at `-Werror`.** Never suppress; fix the cause.
5. **Measured evidence in every claim.** Every claim carries a CI run link or is labelled "not verified".
6. **Honesty labels:**
   - every feature is labelled gated (name the gate), emulated-verified, or not verified on real hardware;
   - never label anything ASI;
   - no quantum speedup claims;
   - never claim an open defect (U1, U2, U3, OPEN-1, OPEN-2) is closed without review.
7. **No weakened assertions, no skipped gates, no deleted gates to get green.** A failing gate means stop and fix.
8. **Clean repo at all times.** Gate logs go under `build/gatelogs/`, never `/tmp`.
9. **`GLOBAL_FORBIDDEN` (in `tools/qemu_runner/boot_test.sh`) is append-only.**
   - Insert new entries BEFORE the final list line.
   - Never put a comment inside its `printf`.
   - Verify after any edit with `source <(sed -n '/^GLOBAL_FORBIDDEN=/,/current FAIL.)"$/p' tools/qemu_runner/boot_test.sh)`; the count must be **77**.
10. **DDR numbers.** Before allocating, confirm the number is unused in BOTH `docs/ddr/` and `docs/decisions/` (`ls docs/ddr docs/decisions | grep DDR-<N>`). Free range: **DDR-1178+**.
11. **`kmalloc` does not zero.** Every new `struct tcb` field needs an explicit initialiser in `sched_create_state`.
12. **One QEMU at a time.** Pre-flight check: `pgrep -f "[q]emu-system-x86_64"`.
13. **Before every commit:** `bash tools/ci/hygiene_check.sh` must pass all eleven checks, and `make smoke-shell` must pass 5/5 locally when the kernel changed.
14. **An address does not identify a binary.** Resolve every RIP against its own binary's `kernel.elf`, using python3 (`tools/ci/sym_at.sh`), never awk.
15. **Performance claims need a denominator.** Under TCG, report static instruction counts, not speedups.

## Hard-won facts (full list in the archive, §INV)

- **Build:**
  - `make image` does not always rebuild after a `-D` flag or a `.c` change: remove the `.o` and verify by **hash**, never by size.
  - `TIMEOUT_S=<n> make smoke-x` is ignored; use `make TIMEOUT_S=<n> smoke-x`.
  - `kputhex` already prints `0x`; `kprintf` does not exist.
- **Size:** `kernel.bin` is **1,540,490 B** against the 1,703,936 B size gate — **163,446 B of headroom** (measured at `a2ac698`). `ci-docstate-check` reads this line; keep size and headroom updated together. The gate is the BIOS stage-2 read window (52 chunks), not a design budget; its basis and every coupled number are in DDR-1183 (operator item 5).
- **NSI:** max defined 117 on this branch (`SYS_FUTEX`); 112–116 are DDR-1178's (#78, #80) and 118 is `SYS_SCHED_CPU` (#91), so next free is 119 (verify against `kernel/syscall/syscall.h`).
- **Boot:** PT_HI is implemented twice, in `boot/stage2/stage2.asm` and `boot/uefi/loader.c`; change both together.
- **`[apfreeze]`** has at least five producers. Resolve its RIP before naming a cause.
- **Container limits:** this container has no `mkfs.fat`, so most smoke gates cannot boot locally. CI is the evidence.

## Open defects (do not claim closed)

- **OPEN-1, route 1:** stated OPEN (DDR-1124).
- **OPEN-2:** the double-dispatch mechanism was fixed and confirmed (DDR-1139). OPEN-2 as a whole is not declared closed.
- **U1/U2/U3:** recorded (DDR-1155), no mechanism named.
- **OPEN-12/13:** open, instrumented.

## Where things are

| Need | File |
|---|---|
| Work order and checkboxes | `docs/V1_MASTER_CHECKLIST.md` |
| What exists, with gates | `docs/FEATURE_STATUS.md` |
| Current state | `SESSION_HANDOFF.md` |
| Design records | `docs/ddr/`, `docs/decisions/` |
| History and invariants | `docs/archive/` |
| Code graph | `tools/graph_mcp/CLAUDE_GRAPH_USAGE.md` |

## Graph usage (6229808666 item 8; DDR-1176: recommended, not mandatory)

- Call `graph_session_primer` at session start.
- `graph_query` / `graph_files` / `graph_deps` are recommended before opening or editing files; plain grep is acceptable (DDR-1176 measured no token win).
- `graph_blast_radius` before a refactor or signature change. Its output is a name-matched superset: a starting list, not a reading list.
- Run `graph_rebuild` after structural changes.
- Do not add a second code-graph tool.
- Report graph_* call counts in every status comment.

## Recent DDRs (one line each; details in the DDR file)

- 1171: rollback on rejection, smoke-rollback.
- 1172: Dependabot triage (`tools/graph_mcp`).
- 1173: dispositions of open docs PRs #27, #31, #36. #31 merged (a dated audit record), #27 closed (superseded by the #45 owner orders and the checklist), #36 closed (a third status list beside `V1_MASTER_CHECKLIST.md` and `FEATURE_STATUS.md`) (see `docs/ddr/DDR-1173-open-docs-pr-dispositions.md`).
- 1174: POSIX signal semantics, smoke-sigsem; post-merge mutant evidence and syscall-exit slow-path analysis in §5–§6.
