# DDR-840 — NSI 87 collision: `SYS_READ_AUDIT` moves to 93

**Status:** accepted
**Date:** 2026-08-06
**Governs:** NSI allocation, `docs/BUILD_TRACKER.md` Section E table
**Relates to:** DDR-834 (credential vault)

## The collision

`docs/BUILD_TRACKER.md` Section E reserved:

```
| `SYS_READ_AUDIT` (Merkle verify, F#76) | 87 | ⬜ |
```

DDR-834 then allocated **87** to `SYS_VAULT_PUT`. When placing the vault I
checked 82–86 (Section E) and 88–90 (prad) and recorded that reasoning in the
DDR — and did not check the Section E table's own last row, which is where the
reservation lived. The check was one line short of the answer.

## Decision

**`SYS_READ_AUDIT` moves to NSI 93.** `SYS_VAULT_PUT` stays at 87.

The vault is shipped and CI-confirmed (`PASS smoke-vault (120s)`, run
30993915008). Renumbering a shipped syscall is a wire-format break: any ring-3
binary built against 87 would silently call a different kernel function. An
unshipped reservation costs nothing to move; a shipped number cannot be moved at
all. The asymmetry decides it.

## Current allocation, recorded here so the next check has one place to look

| NSI | call | state |
|---|---|---|
| 82 / 83 | `SYS_MEMORY_WRITE` / `SYS_MEMORY_READ` | shipped |
| 84 / 85 | `SYS_CHECKPOINT_AGENT` / `SYS_RESUME_AGENT` | shipped |
| 86 | `SYS_APPROVE_CODE_REWRITE` | **open** |
| 87 | `SYS_VAULT_PUT` | shipped |
| 88–90 | `prad` package manager | reserved, deferred post-1.0 |
| 91 | `SYS_VAULT_GET` | shipped |
| 92 | `SYS_SUBMIT_CHILD_ACTION` | shipped |
| **93** | **`SYS_READ_AUDIT`** | **open — reassigned by this DDR** |
| 94+ | free | — |

## The second contradiction fixed alongside

The same Section E table still marked 82/83, 84/85, the spawn-depth cap and the
DAG queue as ⬜ while the rows added when each shipped record them ✅ with CI run
ids. One document, two answers. The table is now the single status source for
Section E and the newer rows point at it.

## The rule this earns

**A reservation is only a reservation if the allocator reads it.** A number
parked in a status table is invisible to someone checking a header, and a header
is invisible to someone reading the table. Allocation lives in ONE place — this
DDR — and the tracker points here rather than restating it.
