# DDR-860 — Group 2 item 7 closes on the hash chain, not a Merkle tree

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Documents a scope decision. No code change.

## The item, and two deviations from it

Item 7 reads *"SYS_READ_AUDIT at NSI 93 (Merkle audit verification)"*.

**(a) `SYS_READ_AUDIT` stays at NSI 37.** It is already shipped there, and
`user/egressaudittest.c`, `user/privacynettest.c` and `user/sovegresstest.c`
parse its record layout. Moving it — or growing its struct, which an earlier
draft did by adding a 32-byte chain field — would have been a ring-3 buffer
overflow in three probes. `SYS_VERIFY_AUDIT` was added at **93** instead,
returning a verdict rather than records (DDR-842).

**(b) The audit log is a linear SHA-256 hash chain, not a Merkle tree.**

## Decision — operator-directed

Keep the chain. Item 7 closes as-is.

The chain already provides the **security** property: any modification to any
entry breaks the chain, and `aether_audit_verify()` detects it and reports the
offending index. A Merkle tree does not detect *more* tampering; it makes
*proving* a single entry's inclusion O(log n) instead of O(n).

At `AETHER_AUDIT_LEN` = 4096 entries that is a proof-size optimisation with no
security gain, against real cost: a new tree structure in kernel memory, a new
gate, and roughly 1.5–2 h of QEMU cycle. The operator reviewed both options and
directed the chain.

Recorded rather than left implicit, because "Merkle" appears in the queue text
and a future reader finding a chain would otherwise reasonably conclude the item
had been missed.

## What would change this

If audit records are ever exported for third-party verification — an auditor
proving one entry without receiving all 4096 — the O(n) proof becomes the
binding constraint and a Merkle root earns its cost.
`aether/kernel/integrity/merkle.py` already exists on the host side and is the
natural place to start.
