# DDR-1017 — Section 3C `ACTION_SPAWN_PROCESS`, and why `SEND_IPC` was not built

**Status:** IMPLEMENTED + gated + M1/M2/M3 mutation-checked on distinct kernel
hashes. Three of eight. Also records a **blocking architectural gap** for
`ACTION_SEND_IPC` rather than skipping it quietly, and fixes a shell-parsing
defect in the DDR-1016 gate.

---

## 1. `ACTION_SEND_IPC` is NOT buildable, and that is the finding

The intended next type was `SEND_IPC`, following DDR-1015's auto-approving shape.
It cannot be built, and the reason is not a missing probe.

**There is no ring-3 IPC surface at all.** `kernel/ipc/ipc.c` exposes

```c
int ipc_send(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, const uint64_t *msg);
int ipc_recv(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, uint64_t *out);
```

— kernel-internal, capability-gated, taking a `struct cap_table *` no ring-3
caller has. `grep` finds **no `SYS_IPC_*` in `syscall.h`**. So the DDR-1013 §2.1
architecture (kernel = policy engine, agent = executor) has no executor here:
**an approved `ACTION_SEND_IPC` could not be carried out by any agent.**

That is a different situation from the six types deliberately absent from the
enum (`EXEC_CODE`, `PARSE_DOCUMENT`, `BROWSE_WEB`, `CAPTURE_FRAME`,
`SCAN_ENVIRONMENT`, `QUERY_SCENE`), which `aether.h` omits precisely so an agent
cannot submit something nothing implements. `SEND_IPC` **is** in the enum, so it
can be submitted and approved today, with no way to act on it.

**What building it would require:** a new NSI (97 is next free, §INV.14) wrapping
`ipc_send` for a ring-3 caller, plus a capability check and an endpoint the agent
can name. That is new kernel ABI, not a probe, and it is a security-surface
decision — an agent-to-agent channel is exactly the sort of thing DDR-842 gates.
**Not started, deliberately, and not assumed harmless.** `QUERY_MEMORY` needs
checking the same way before anyone budgets it as "one more probe".

`SPAWN_PROCESS` was built instead because its ring-3 executor already exists
(`SYS_FORK`) and is already proven by `smoke-spawndepth`.

## 2. Not already covered

Checked before writing anything: `spawndepthtest.c` (DDR-838) exercises the
**fork depth cap** and never calls `SYS_SUBMIT_ACTION`; `actiondagtest.c` submits
only `ACTION_PRINT`. **No probe submits `ACTION_SPAWN_PROCESS`.**

## 3. The effect is asked of the kernel, not asserted by the probe

`SPAWN_PROCESS` is force-pending (DDR-842 S4), so this follows DDR-1016's shape:
the verdict must stay `AE_PENDING` and nothing may be spawned for it. But the
filesystem cannot witness a process, so the probe calls
`wait4(-1, &st, WNOHANG)` and prints the raw return code:

| `post=` | meaning | verdict |
|---|---|---|
| `-10` | `-ECHILD`, this process has no children | **required** |
| `-11` | `-EAGAIN`, a child exists and has not exited | a fork happened |
| `> 0` | a child exited and was reaped | a fork happened |

The number comes from `sched_find_child` walking the real thread ring. "I did not
fork" would be the probe describing itself, which is worth nothing — this repo
has hit that failure mode repeatedly, and §5 below is this DDR's own instance.

## 4. The control, and the dead-arm class again

`post=-10` is evidence only if fork and reap work in **this** boot; if fork were
broken, a mutant that forked would also report no child. So the probe forks a
child of its own — no action involved — and reaps it, requiring **both** the pid
and the exit status to match.

Leaning on `smoke-spawndepth` for this would not do: that gate proves the fork
*cap* in *its own* boot, and a gate that borrows another gate's boot is not
measuring its own.

**`ctrl` had to be made computed.** The first draft called `fail()` on each
control mismatch and printed a literal `ctrl = 1`. A broken control therefore
never reached the printed line, so the gate's `ctrl` check **could never fire**
and the field could not vary — the same dead arm DDR-1016 §5 found in its `st`
check. Now `ctrl_ok = (reaped == kid && cst == CHILD_EXIT)` and a broken control
prints `ctrl=0`. **M3 exists specifically to prove that arm is live.**

Worth naming as a class, since it has now appeared twice in two DDRs: **a field
whose only reachable value is the passing one is decoration, not measurement.**

## 5. A parsing defect, in this gate and latent in DDR-1016's

The gate first failed with:

```
[actionspawn] PRADYOS_ACTIONSPAWN_OK id=258 st=1 ctrl=1 post=-10
[actionspawn] FAIL — verdict st=-10, expected 1 (AE_PENDING)
```

The measured line was correct; the **gate** was wrong. `${ln##*st=}` strips the
longest prefix ending in `st=` — and `post=` ends in `st=`, so `st` was being
read out of `post`. Fixed by anchoring every field on its leading space
(`${ln##* st=}`).

**DDR-1016's gate was hardened the same way and re-run**, though it parses
correctly today: it does so only because none of its field names happens to end
in `st`, which is luck, not design — and "it works today" is exactly the
reasoning `ci-start-align-check` refuses to accept (DDR-1016 §7).

## 6. Measured

Baseline kernel **`30658af9358ab055`**, `-Werror` clean, **1,118,602 B** against
the 1,572,864 B gate.

```
[actionspawn] PRADYOS_ACTIONSPAWN_OK id=258 st=1 ctrl=1 post=-10
[actionspawn] PASS — force-pending, and no process appeared for it
```

| # | mutation | kernel | result | arm |
|---|---|---|---|---|
| M1 | probe forks on a PENDING action | `5cd2db8a5d2a68ca` | `post=45` → FAIL | `post` only |
| M2 | kernel drops `SPAWN_PROCESS` from `aether_action_forces_pending()` | `a09869767ad0ef1a` | `st=2` → FAIL | `st` only |
| M3 | control child exits with the wrong status | `1ea29f035d1b296f` | `ctrl=0` → FAIL | `ctrl` only |

Four distinct hashes, **each mutant failing exactly one arm**, so no arm is
carrying another. M1 and M2 mutate the **system**; M3 mutates the **gate's own
control** and exists only to show that arm can fail — a distinction worth keeping
explicit rather than presenting three mutants as three equal defect classes.

M1 was **re-run against the shipped probe** after the §4 control refactor: an
earlier M1 result (`38d70d5ca7a8e1bf`, `post=42`) was measured on a draft that no
longer exists, and a mutation result on code that was not shipped is not a
result.

Gate suite on the baseline, one hash verified before and after each run:
`smoke-actionspawn`, `smoke-actiondel`, `smoke-actionread`, `smoke-spawndepth`,
`smoke-shell` (73-pattern scan clean), `smoke-blkmq`,
`smoke-rqstress-liveness`, `smoke-blk-integrity` — all PASS.
`hygiene_check.sh` ALL THREE PASSED (161 gates / 10 shards / 7 excluded;
64 probe ELFs; 46 entry points).

## 7. Five remain, and two of them need a decision first

`REWRITE_AGENT_CODE` and `EVOLVE_GENOME` are force-pending and follow this file
directly — both write files, so ring-3 executors exist.
`PROPOSE_HYPOTHESIS` follows DDR-1015's shape (an SFS write).

**`SEND_IPC` and `QUERY_MEMORY` are blocked on §1** and should not be counted as
probe work. Neither is claimed done, and Section 3C is **3 of 8**.
