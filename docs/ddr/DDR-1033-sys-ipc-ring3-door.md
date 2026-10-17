# DDR-1033 — `SYS_IPC_SEND` / `SYS_IPC_RECV` (NSI 98/99): the ring-3 IPC door

Status: **IMPLEMENTED + GATED + mutation-checked (M1/M2/M3)**
Kernel `715520928e873aab`, `-Werror` clean. Gate `smoke-sendipc` (shard 7, strict).
(§1–§4 landed as design before the code; **§4's arm B was passing for the wrong
reason and had to be rebuilt — see §6.**)
Operator instruction on PR #17, 2026-08-31. Closes the `ACTION_SEND_IPC` gap
DDR-1017 recorded as blocked.

---

## 1. What DDR-1017 got right, and what it understated

DDR-1017 deferred `ACTION_SEND_IPC` because "ipc_send/ipc_recv are
kernel-internal and capability-gated, there is no `SYS_IPC_*`, so an approved
SEND_IPC has no executor in any ring". Re-reading the code, that is right about
the door and understates how much already works:

- `ipc_send` / `ipc_recv` exist and are complete (`kernel/ipc/ipc.c`) — a
  one-slot synchronous endpoint with a proper lock and a blocked-receiver wake.
- They are **already capability-gated**: each checks
  `cap_authorize(caps, h, RES_IPC, e->res_id, CAP_IPC_SEND|RECV)`, and
  `CAP_IPC_SEND` (bit 7) / `CAP_IPC_RECV` (bit 8) already exist in `cap.h`.
- `ipc_recv` already has DDR-961's bounded form, returning `-ETIMEDOUT`.

So the missing pieces are exactly two: **a ring-3 door**, and **an addressing
scheme** — nothing today lets one agent name another's roster slot.

## 2. Addressing

One `struct ipc_endpoint` per roster slot, `g_agent_ep[AGENT_ROSTER_N]`, living
in `sys_aether.c` where `AGENT_ROSTER_N` is already in scope. The ring-3 address
of an agent is simply its **roster slot index** — the same identifier
`SYS_AGENT_ROSTER` and `SYS_AGENT_METRICS` already use, so no new namespace is
invented.

## 3. Two layers of authority, and an honest limit on the second

**Layer 1 — `is_ipc` on `struct tcb`.** May this process use the door at all.
Set by the kernel at spawn, never mintable by the process, following the
`CAP_MEMORY`/`is_memory` (DDR-836) and `CAP_REWRITE`/`is_rewrite` (DDR-842)
pattern exactly. Explicitly zeroed in `sched_create` per §NON-NEGOTIABLE 10 —
`kmalloc` does not zero.

**Layer 2 — the capability handle.** `ipc_send`/`ipc_recv` demand one, so the
kernel mints it at spawn beside `is_ipc` and stores it as `t->ipc_cap`. A
process cannot mint its own: `cap_create` is kernel-side, and granting it at
spawn is what keeps this out of self-escalation territory.

**The limit, stated rather than implied:** every slot endpoint shares one
`res_id`, so the capability grants "IPC at all", **not** "send to slot 3 but not
slot 5". It is a real check — a process without the handle is refused by
`cap_authorize` — but it is **coarse**, and it does not express per-slot policy.
Per-slot `res_id`s and one handle per slot are the natural extension if policy
is ever wanted; that is a policy decision, not a missing mechanism, and it is not
being invented here without a caller that needs it.

## 4. The gate, and how the refusal arm is made reachable

`is_ipc` is a **per-process** flag, so a single process cannot exercise both the
granted and refused paths. The kernel therefore spawns the probe **twice** — once
with `is_ipc` granted, once without — and the probe reports the return code of
its first call either way.

| arm | sentinel | what it would catch |
|---|---|---|
| A | `PRADYOS_IPC_GATE rc=0` | the granted process is refused: the door does not open |
| B | `PRADYOS_IPC_GATE rc=-1` | **the un-granted process is let through** — the gate is decorative |
| C | `PRADYOS_IPC_RT w0=… w3=…` | the message round-trips, first **and last** word: a truncated 32-byte copy fails |
| D | `PRADYOS_IPC_SLOT rc=-22` | slot bounds are unchecked |

Arm B is the one that matters and the reason for the double spawn: without it,
`is_ipc` could be hardcoded to 1 and every other arm would still pass. Arm C
asserts both ends of the payload because `IPC_MSG_WORDS` is 4 and a copy that
moved only the first word would otherwise read as success.

## 5. Mutation results

| mutant | change | kernel | outcome |
|---|---|---|---|
| — | clean | `715520928e873aab` | **PASS**, all five sentinels |
| **M1** | `sys_ipc_send` drops the `is_ipc` check | `8853aecb812532ba` | **FAIL at arm B** — *both* processes print `rc=0`; the required `rc=-1` never appears |
| **M2** | copy one word instead of four | `5d1805213ae89c84` | **FAIL at arm C** — `payload did not round-trip` |
| **M3** | slot bound widened past the array | `484e9b390aed1d7a` | **FAIL at arm D** — `out-of-range slot was accepted` |

M3 is worth reading carefully: with the bound widened, a **ring-3 integer indexes
`g_agent_ep[99]` directly**. The check is not input validation for tidiness — it
is the only thing between userspace and an out-of-bounds array read.

## 6. Arm B was passing for the wrong reason, and the first M1 proved it

The design said the un-granted process would be spawned with the door simply not
granted. That is what was built, and **the first M1 run passed** — a mutant that
defeated the `is_ipc` check *still* produced `rc=-1` for the un-granted process.

The reason: that process held no capability either, so `ipc_send`'s own
`cap_authorize` refused it regardless. **`is_ipc` could have been deleted
outright and the gate would still have gone green** — the dead-arm class this
project has now hit seven times, and the first instance found by a mutant rather
than by reading.

The fix makes the two layers separable in the fixture, not just in the design:
the deny process is now spawned with `ipc_grant()` **and then has `is_ipc`
cleared**, so it holds the capability and lacks only the door. Arm B is then a
test of the door specifically, and the re-run M1 fails it — both processes print
`rc=0`.

That this was only discoverable by mutation is the point worth carrying: two
independent checks in series will each mask the other's absence, and a fixture
that trips both at once cannot tell you which one is load-bearing.

## 7. Gates

`smoke-sendipc`, `smoke-aether`, `smoke-agents`, `smoke-shell` all PASS;
`hygiene_check.sh` all three PASSED.

## 8. Not done

`ACTION_SEND_IPC` now has an executor, but the AETHER action path does not yet
*call* it — an approved `SEND_IPC` still has no automatic effect. Wiring the
action verdict to this syscall is a separate change, and is recorded in the
pre-launch checklist rather than claimed here.
