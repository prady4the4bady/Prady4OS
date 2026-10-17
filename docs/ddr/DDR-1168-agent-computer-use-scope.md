# DDR-1168 — Agent computer-use layer: scope (design only, no code)

**Status:** DESIGN. No code, no gate, `kernel.bin` unchanged. Written before
any implementation (NON-NEGOTIABLE 5). Operator instruction: PR #44 comment
6177173413 (OWNER), item 3; vision in `docs/VISION.md` §1–§2 (DDR-1169).

## 1. Rule every item obeys

Each item is reached **only** through a capability-checked syscall. The
capability is minted kernel-side (never by the holder), zeroed in
`sched_create_state` (NON-NEGOTIABLE 10), checked with `cap_authorize`, and
every grant, refusal and action writes an audit record. No item may be
reachable by a process that was not given its capability at spawn.

## 2. Items

| # | Item | Capability | Depends on | Gate (to build) |
|---|---|---|---|---|
| 1 | **Per-task budgets**: syscalls, CPU ticks, bytes written, egress bytes, wall time, per task (not only per agent, as `AETHER_RATE_MAX` is today). Exhaustion stops the task and is audited. | kernel-owned; not grantable | none | budget-exhaust arm + a mutant that skips the charge |
| 2 | **Kill switch**: operator-only stop of one task or all agent tasks. `SYS_KILL_AGENT` exists; the gap is a whole-fleet stop and that it lands in unbounded kernel waits (DDR-1090 fixed SIGKILL there). | `is_sovereign` only | none | kill-during-block arm |
| 3 | **Egress**: per-task allowlist + byte budget on top of DDR-1070/1091/1141. UDP stays per-send if ever built (DDR-1091 §3). | `CAP_NET` + task scope | none | refused-destination arm with host-side responder log (DDR-1141 shape) |
| 4 | **Rollback**: changes a task makes are journaled; a rejected or failed task is undone. Starts with SFS file writes (append-only SFS makes undo = truncate-to-recorded-size or unlink-created). | kernel-owned | SFS write journal | reject-then-check-file-unchanged arm |
| 5 | **Structured UI-state reading**: the compositor publishes window tree, titles, focus and geometry as structured records (not pixels). Extends `PRADYOS_WM_GEOM`. | new `CAP_UI_READ` | compositor | read-own-vs-other-window arm |
| 6 | **Input injection via the compositor**: synthetic key/pointer events delivered by the compositor to one target window, never to the raw PS/2/USB rings. | new `CAP_UI_INPUT` | item 5 | inject-to-unfocused-window-refused arm |
| 7 | **Sandboxed shell**: PRISM in a child with a dedicated SFS subtree as root, budget from item 1, no inherited capabilities. | `CAP_EXEC`-scoped | items 1, 4 | escape-the-root-refused arm |
| 8 | **Sandboxed browser harness**: fetch + parse in a sandboxed process. **Depends on TLS**, which is blocked on a trust anchor (DDR-1104 §1, DDR-1059). Plain-HTTP-only would be the DDR-1059 shape and is refused. | `CAP_NET_BROWSE` (DDR-1149) | TLS + trust anchor | not buildable until TLS exists |
| 9 | **Adversarial gates**: each item's gate asserts the attack fails (`smoke-invariants` style), not only that the feature works. | — | each item | one per item |

## 3. Approval

Starting a task and any code rewrite stay forced `PENDING`
(`aether_action_forces_pending`: SPAWN_PROCESS, DELETE_FILE,
REWRITE_AGENT_CODE, EVOLVE_GENOME). Nothing here removes a type from that list.

## 4. Build order

1 → 2 → 3 → 4 → 5 → 6 → 7; 8 waits on TLS; 9 lands with each item. Each item
gets its own DDR, mutants and gate before it is called done.

## 5. Not claimed

Nothing in this file is built. No capability bit is allocated here; numbers
are assigned in the item's own DDR.
