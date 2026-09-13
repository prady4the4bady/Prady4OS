# DDR-1015 — Section 3C `ACTION_READ_FILE`, end to end

**Status:** IMPLEMENTED + gated + M1 mutation-checked. The first of the eight
Section-3C action types, built to the shape DDR-1013 §2.1 specified.

---

## 1. What "implement a 3C action type" turned out to mean

DDR-1013 §2 corrected a wrong reading of this work — that the kernel queues and
approves action types nothing implements, and should reject them. It does not:
`agent_base.c` states the architecture at the top of the file, *"it never holds
the authority to act, the kernel does"*, and there is no kernel-side executor for
`ACTION_WRITE_FILE` either. **The kernel is the policy engine; the agent is the
executor.** So a 3C type is implemented in ring 3.

The full pipeline, and the order is the whole point:

1. **propose** — `SYS_SUBMIT_ACTION(ACTION_READ_FILE, path, len)` → `action_id`
2. **wait for the verdict** — `SYS_POLL_RESULT(action_id)` until it leaves `AE_PENDING`
3. **execute, and only then** — `open`/`read` with the agent's own authority
4. **verify the bytes**

`ACTION_READ_FILE` is not in `aether_action_forces_pending()`, and sovereign mode
is the ADR-026 D2 default (`aether_queue.c:37`), so it auto-approves and the probe
needs no second privileged actor. The four force-pending types
(`SPAWN_PROCESS`, `DELETE_FILE`, `REWRITE_AGENT_CODE`, `EVOLVE_GENOME`) do, and
their gates must assert the action stays **PENDING** — DDR-1013 §2.1 recorded that
split so nobody writes a gate asserting the opposite of DDR-842's design.

## 2. The wire constant is pinned, because the last one drifted

`user/actionreadtest.c` hand-copies `ACTION_READ_FILE = 5` across the ring
boundary. DDR-1013 §1 found `actiondagtest.c` had drifted to a wrong constant with
no gate able to see it, so this commit adds

```c
_Static_assert(ACTION_READ_FILE == 5, "action wire format: READ_FILE is 5");
```

next to the three existing pins in `aether.h`. If the enum shifts, **the kernel
stops building** — the only cross-check the build currently has between a probe's
constants and that header.

## 3. The gate asserts the effect, not the sentinel

```
[actionread] PRADYOS_ACTIONREAD_OK id=258 n=25 first=P
[actionread] PASS — proposed, approved, then read
```

`n=25` is exactly `len("PRADYOS filesystem works!")`, the content of `/HELLO.TXT`
on the FAT image; `first=P` is its first byte. The gate parses both out of a
pinned capture and fails on either.

**M1 — claim success without reading.** Kernel `6edc2e3889fd847b` (distinct hash).
The mutant skips the read and, **deliberately, still sets `first='P'`**, so only
the byte-count arm can catch it:

```
[actionread] PRADYOS_ACTIONREAD_OK id=258 n=0 first=P
[actionread] FAIL — approved read returned 0 bytes
```

Caught, and it shows the count arm is load-bearing rather than decorative — the
sentinel and the first-byte check both passed on a probe that never read
anything.

## 4. Measured

Kernel **`b0e4ccb83d4bb7ac`**, `-Werror` clean, **1,106,314 B** against the
1,572,864 B gate.

| gate | result |
|---|---|
| `smoke-actionread` | PASS (`n=25 first=P`) |
| M1 (read skipped) | **FAILS**, as designed |
| `smoke-actiondag` | PASS |
| `smoke-aether` | PASS |
| `smoke-fat32-multicluster` | PASS |
| `smoke-shell` | PASS |

`ci-shard-check` OK (159 gates / 10 shards / 7 excluded);
`ci-probe-rodata-check` OK (**62** ELFs — the new probe is in it).

## 5. What this gate does NOT prove — the ordering

**It proves the pipeline ran and the read happened. It does not prove the read
happened *after* the approval.** A probe that read the file first and submitted
afterwards would print an identical line, and nothing in the serial output
distinguishes the two orders.

That matters, because the ordering *is* the authority property — it is the whole
reason the pipeline exists. Recorded as unmeasured with its reason, the same
treatment DDR-998 gave its M3 and DDR-1004 its SKIP branch, rather than left for
a reader to assume.

**What would measure it:** submit an action the policy will *reject* and assert
the read does not happen — the one case where the two orders give different
observable outcomes. That needs a rejecting policy path in the gate (manual mode,
or a force-pending type with no approver), and it is unbuilt. It is the natural
second arm and the natural shape for the `DELETE_FILE` gate, which needs a
PENDING assertion anyway.

## 6. Seven remain

`DELETE_FILE`, `SEND_IPC`, `QUERY_MEMORY`, `REWRITE_AGENT_CODE`,
`PROPOSE_HYPOTHESIS`, `RUN_EXPERIMENT`, `EVOLVE_GENOME`. Three of those are
force-pending and need the PENDING-asserting shape; the rest follow this file
directly. **No claim is made that Section 3C is complete** — one of eight is
done, and the pattern is now established rather than described.
