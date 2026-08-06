# DDR-842 — code-rewrite gate (NSI 86), hash-chained audit (NSI 93), 3C action types

**Status:** accepted
**Date:** 2026-08-06
**Governs:** `kernel/aether/aether_audit.c`, `kernel/syscall/sys_rewrite.c`,
`kernel/syscall/sys_audit.c`, `enum aether_action`
**Covers:** Group 2 items 6, 7, and the unblocked part of item 8

---

## Item 6 — `SYS_APPROVE_CODE_REWRITE` (86) + `CAP_REWRITE` (1<<21)

S4 makes the human gate structural for code rewrites; S8 requires
`CAP_SOVEREIGN` on any `skill.md`/code write. The spec is explicit that
`CAP_REWRITE` **"always requires `CAP_SOVEREIGN` co-approval, never
auto-granted"**.

**Co-approval means BOTH bits, checked together, in one place.** The syscall
requires `is_rewrite && is_sovereign`. A holder of `CAP_REWRITE` alone is
refused, and — the arm that actually matters — a holder of `CAP_SOVEREIGN` alone
is *also* refused. If sovereignty alone sufficed, `CAP_REWRITE` would be
decoration, and every sovereign process would silently be a code-rewrite
authority.

Approval targets a **pending `ACTION_REWRITE_AGENT_CODE`** in the DAG queue and
is refused for any other action type: a call that could approve an arbitrary
action id would be a general-purpose approval bypass wearing a specific name.

---

## Item 7 — hash-chained audit + `SYS_READ_AUDIT` (93)

The audit log is append-only today: there is no erase or rewrite syscall. That
defends against a *user-space* attacker. It does nothing against a corrupted or
edited log, because **nothing can tell an intact log from a modified one** — S5
claims "append-only audit + Merkle ledger" and only the first half shipped.

Each entry gains a 32-byte chain value:

```
chain[i] = SHA-256( chain[i-1] || timestamp || agent_pid || action_type
                                || action_id || result )
chain[-1] = 32 zero bytes
```

Verification recomputes the chain from the oldest retained entry and reports the
**index of the first mismatch**, not just a boolean. An operator asking "was the
log tampered with?" needs "yes, at entry 1,204", because that locates the event
being hidden.

### Honest limit, stated because it would otherwise look stronger than it is

The log is a **circular buffer**, so verification covers the retained window, not
all history. After a wrap, `chain[oldest]` has no in-log predecessor to check it
against, and the recomputation is anchored at that entry rather than at a genesis
value. **A wrap is therefore a real gap in the chain of custody**, which is
exactly why `AETHER_AUDIT_WRAP` is emitted to serial — the wrap event is the
audit trail for the missing audit trail. A durable ledger that survives wrap is
the SFS-backed science ledger (F#76), which is a different mechanism and is not
claimed here.

`SYS_READ_AUDIT` is `CAP_SOVEREIGN`: the log records which agent did what, and
an agent able to read the whole log learns the operator's approval patterns and
every other agent's activity.

### The tamper arm needs kernel-side fault injection, and that is the point

Ring 3 has no write path into the log — by design, and that is what S5 asserts.
So the gate cannot corrupt an entry from user space, and a gate that only ever
verifies an intact log proves nothing: a `verify()` that returns OK
unconditionally would pass it.

The tamper is injected in the kernel behind the DDR-804 probe flag
(`QEMU_PROBES=audittamper`), flipping one byte of one committed entry. It exists
only when that flag is set, fails closed, and is the only way to prove the
verifier can fail.

---

## Item 8 (partial) — eight new action types, APPENDED

`ACTION_READ_FILE` · `ACTION_DELETE_FILE` · `ACTION_SEND_IPC` ·
`ACTION_QUERY_MEMORY` · `ACTION_REWRITE_AGENT_CODE` ·
`ACTION_PROPOSE_HYPOTHESIS` · `ACTION_RUN_EXPERIMENT` · `ACTION_EVOLVE_GENOME`

**Appended to `enum aether_action`, never inserted** — the action type crosses
the ring boundary in every audit record and queue entry, so an insertion
renumbers a wire format exactly as DDR-832 describes for `enum aether_result`.
`_Static_assert`s pin the pre-existing values.

**Force-PENDING set.** `ACTION_DELETE_FILE`, `ACTION_REWRITE_AGENT_CODE` and
`ACTION_EVOLVE_GENOME` join `ACTION_SPAWN_PROCESS` as never auto-approved, even
in sovereign mode — S4 names all of them as requiring a human gate. This is
enforced in the same place `ACTION_SPAWN_PROCESS` already is, so there is one
list rather than two that must agree.

### Six types deliberately NOT built, with reasons

`ACTION_CAPTURE_FRAME`, `ACTION_SCAN_ENVIRONMENT`, `ACTION_QUERY_SCENE` are
post-L7 by the spec and need `CAP_SCENE` plus camera/SLAM paths that do not
exist. `ACTION_PARSE_DOCUMENT` needs a 64 MiB local OCR model with no shipping
path. `ACTION_EXEC_CODE` needs a sandboxed interpreter — a subsystem, not an
action type. `ACTION_BROWSE_WEB` needs a headless browser and network egress, a
hard dependency on the deferred cloud bridge (DDR-793).

Declaring these enum values without enforcement would be worse than omitting
them: an agent could submit one and the kernel would queue an action nothing
implements. They are omitted until their subsystem exists.

---

## The rule this earns

**Append-only is not tamper-evident.** Removing the write path stops one
attacker and proves nothing about the bytes on the page; only a chain that can be
recomputed distinguishes an intact log from an edited one, and only a gate that
can make verification FAIL proves the verifier works.
