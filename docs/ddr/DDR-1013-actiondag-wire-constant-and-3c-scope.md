# DDR-1013 — a mislabelled action constant, and what "implement a 3C action type" actually means

**Status:** FIX (one constant) + SCOPE CORRECTION. The scope half records an
analysis I got wrong and caught before acting on it, because DDR-982 §5.5's
framing invites the same error.

---

## 1. The constant

`user/actiondagtest.c:35` read:

```c
#define ACTION_PRINT 1
```

`aether.h` pins the wire format with assertions, and **1 is not PRINT**:

```c
enum aether_action {
    ACTION_NONE = 0, ACTION_WRITE_FILE, ACTION_PRINT, ACTION_SPAWN_PROCESS, …
};
_Static_assert(ACTION_WRITE_FILE    == 1, "action wire format: WRITE_FILE is 1");
_Static_assert(ACTION_PRINT         == 2, "action wire format: PRINT is 2");
```

So every action `smoke-actiondag` submitted was queued and **audited as a file
write**, while every line of the probe called it a print. Fixed to `2`.

### 1.1 Why no gate caught it, which is the part worth keeping

`smoke-actiondag` asserts two sentinels (`PRADYOS_ACTIONDAG_OK`,
`PRADYOS_ACTIONDAG_SUBMIT_OK`) and one forbidden string. The DAG logic it tests —
parent/child ordering, orphan rejection, TTL — is **type-agnostic**:
`aether_submit` never branches on `action_type` except through one predicate.

That predicate is `aether_action_forces_pending()`, and it is the near-miss:
`WRITE_FILE` and `PRINT` are **both outside** the force-pending set, so the two
behave identically on the only type-sensitive path in the whole submit flow. The
bug was invisible because of a coincidence, not because it was harmless. Add
`WRITE_FILE` to that set, or point the probe at a type inside it, and the gate
changes meaning with nothing to announce it.

A probe's constants are a wire format, and this one had drifted from the header
that asserts it. There is no cross-check between `user/*.c`'s hand-copied NSI and
action numbers and the kernel headers — `agent_base.c` carries the same shape of
duplicated block. That is worth a checker, and it is **not built here**: it is a
build-system change, and this file's evidence is one wrong constant, not a
measured pattern.

## 2. SCOPE CORRECTION — what implementing a 3C action type requires

`aether.h` declares eight Section-3C action types (`READ_FILE`, `DELETE_FILE`,
`SEND_IPC`, `QUERY_MEMORY`, `REWRITE_AGENT_CODE`, `PROPOSE_HYPOTHESIS`,
`RUN_EXPERIMENT`, `EVOLVE_GENOME`). A grep shows them referenced **nowhere** but
the enum and the force-pending predicate.

**I read that as a hole and was wrong.** The reasoning I nearly acted on: the
kernel queues and approves an action type that nothing implements, therefore
`aether_submit` should reject unimplemented types. That inverts the architecture.

`agent_base.c` states it at the top of the file:

> "It turns a model 'response' into an action, PROPOSES it to the kernel
> (`SYS_SUBMIT_ACTION`), waits for the policy verdict (`SYS_POLL_RESULT`), and
> only then executes — **it never holds the authority to act, the kernel does**."

**The kernel is the policy engine; the agent is the executor.** There is no
kernel-side executor for `ACTION_WRITE_FILE` either. So a declared type with a
policy entry and no ring-3 implementer is an unused vocabulary entry, not an
unguarded path — and adding submit-time rejection would have broken the
force-pending policy that already names three of those eight as human-gated.

Recording the wrong reading as well as the right one, because it is exactly the
inference DDR-982 §5.5 sets up when it asks whether declaring a type without
"enforcement" is safe. The answer depends on what enforcement means here, and it
means *policy*, which already exists.

### 2.1 So the Section 3C rows are ring-3 work, and they are tractable

Implementing `ACTION_READ_FILE` means, in full:

1. a ring-3 probe that submits the type, polls for the verdict, and **on approval
   performs the read with its own authority** (an ordinary `open`/`read`);
2. a gate asserting the whole pipeline — submitted, approved, executed, and the
   bytes correct — not merely that a sentinel printed;
3. for the four force-pending types (`SPAWN_PROCESS`, `DELETE_FILE`,
   `REWRITE_AGENT_CODE`, `EVOLVE_GENOME`) the gate must assert the action stays
   **PENDING** in sovereign mode and needs an explicit approve. A gate that
   expected auto-approval there would be asserting the opposite of DDR-842's
   design.

That last point is why "gate per type" in the backlog is not boilerplate: the
eight types fall into two policy classes and the gates differ.

## 3. MEASURED

Kernel **`ba6ac01fe015b2a4`**, `-Werror` clean, `kernel.bin` 1,102,218 B.

| gate | result |
|---|---|
| `smoke-actiondag` | PASS |
| `smoke-auditchain` | PASS |
| `smoke-aether` | PASS |
| `smoke-shell` | PASS |

`ci-shard-check` OK (158 gates / 10 shards / 7 excluded);
`ci-probe-rodata-check` OK (61 ELFs).

**The prediction was stated before the run and held.** §1.1 argued the change is
behaviourally inert for every gate: the DAG path does not branch on the type, and
`WRITE_FILE` and `PRINT` coincide on `aether_action_forces_pending()`. Checked
rather than assumed — no gate in the Makefile asserts on an action-type number,
and `aether_audit.c` prints no type string a gate matches. A failure here would
have meant the analysis was wrong, which is why the expectation was written down
first.

No claim is made that the 3C types are implemented, or that this file starts
them. §2.1 is the specification for whoever does.
