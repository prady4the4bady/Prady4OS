# DDR-1170: Per-task budgets (time, action, byte), kernel-enforced

Status: design, committed before code (§NON-NEGOTIABLE 5). Issue #45 Block B item 1.

## 1. What exists today

- `AETHER_RATE_MAX` (60 syscalls per 100 ticks, `aether_mem.c:61`) is a
  **rate**, not a budget: it never runs out, and an agent that spins in ring 3
  without making syscalls is never charged at all.
- `mem_limit` caps the size of one resource (memory) and does not cap cost.
- Nothing bounds how long an agent task runs, how many actions it may
  propose, or how many bytes it may emit. `docs/FEATURE_STATUS.md` records
  "Per-task budgets: not started".

## 2. What "token" means here, stated honestly

The row says "time, token, byte". The kernel **cannot observe model tokens**:
inference runs off-box (Ollama on the host, DDR-1110 §3), reached over a proxy
socket, so the kernel sees bytes, not tokens, and an agent could report any
token count it liked. A kernel-enforced token budget would therefore be the
DDR-1059 shape, a control that reads stronger than it is.

So the three kernel-enforced budgets are:

| Budget | Unit | Why it is the honest form |
|---|---|---|
| time | PIT ticks since the task started | wall-clock bound, charged even with no syscalls |
| actions | `SYS_SUBMIT_ACTION` / `SYS_SUBMIT_CHILD_ACTION` calls | the agent's unit of proposed work, counted by the kernel |
| bytes | bytes written by `write`/`writev` (any fd) plus `SYS_SOCK_WRITE` | the agent's output volume, including everything sent to a model endpoint, so it bounds the prompt side of inference |

Model tokens, if wanted, are a userspace accounting on top of the byte
budget, which bounds them from above. Not claimed as kernel-enforced.

## 3. Interface

- `struct task_budget { uint64_t max_ticks; uint64_t max_bytes; uint32_t max_actions; uint32_t _pad; }`.
  Zero in a field means "no limit on that axis".
- **NSI 107 `SYS_SPAWN_TASK(task, slot, const struct task_budget *b)`**, sovereign only.
  A new syscall rather than widening `SYS_SPAWN_AGENT`. Measured: all three
  callers of `SYS_SPAWN_AGENT` use the three-argument `nsi` stub, and
  `prism.c:965` passes a **string pointer** in `a1`. So no spare argument
  register is reliably zero, which is DDR-1098 §2's lesson again.
- The budget is applied **before the child first runs**: `elf_load_args`
  returns the thread BLOCKED and the hook sets the fields before
  `sched_unblock` (`main.c` spawn hook). A budget set after the child had run
  would leave a window in which it is unbounded.
- `SYS_SPAWN_AGENT` stays exactly as it was, with no budget.
- Restricted to the sovereign: an agent spawning a sub-task with a fresh
  budget would escape its own. Nesting (child at most equal to the parent's
  remainder) is recorded as a follow-up, not built.

## 4. Enforcement

- New TCB fields: `bud_max_ticks`, `bud_max_bytes`, `bud_max_actions`,
  `bud_start`, `bud_bytes`, `bud_actions`, `bud_tripped`. All are explicitly
  initialised in `sched_create_state` (§NON-NEGOTIABLE 10). The limits are
  **not inherited across fork**, matching every other authority flag.
- Charging points and the check that runs at each:

  | Point | Charges | Check |
  |---|---|---|
  | `sys_submit_action` / `sys_submit_child_action` | +1 action **before** the submission is queued | refused past the limit |
  | `fd_write_user` | bytes charged **before** the copy | refused past the limit |
  | `sys_sock_write` | bytes charged **before** the copy | refused past the limit |
  | time | ticks since `bud_start` | checked at syscall entry (beside the rate check) **and** on the timer's ring-3 return path |

  The timer-path check matters: without it, a task spinning in ring 3 never
  reaches a syscall and would never be stopped. When the time budget is blown
  there it raises `SIGKILL`, which `signal_deliver` acts on, on the same frame.
- **On exhaustion the task is killed, not merely refused.** The kill is
  audited once as `AR_BUDGET_KILL`, appended to the enum per DDR-832, with
  `action_id = (kind << 56) | used`, where kind is 1 = time, 2 = bytes,
  3 = actions. The task's pending queue entries are then dropped
  (`aether_drop_pid`).
- Killing rather than refusing: a refused write returns an error the agent can
  ignore and simply retry, so a budget that only refuses is a rate limiter
  again. The rate limiter already kills for this same reason.

## 5. Gate: `smoke-taskbudget` (new, strict)

- A probe ELF `budgettest` runs as sovereign and spawns four `agent_base`
  tasks through `SYS_SPAWN_TASK`. `agent_base` gains three test behaviours,
  selected by its task string:
  - `bud-time` spins in ring 3 forever, making no syscalls;
  - `bud-bytes` writes 1 KiB blocks to a pipe it then drains;
  - `bud-actions` submits actions repeatedly.
- Arms (exact values, DDR-1044):

  | Arm | Task | Budget | Expected |
  |---|---|---|---|
  | T | `bud-time` | 50 ticks | `AR_BUDGET_KILL` with kind 1 for that pid |
  | B | `bud-bytes` | 4096 B | kind 2, `used` > 4096 |
  | A | `bud-actions` | 3 | kind 3, `used` = 4 |
  | C (control) | `bud-bytes` | 1 MiB | finishes its fixed 8 KiB of writes and exits normally; **no** `AR_BUDGET_KILL` for its pid |

  The control is what makes T, B and A mean anything: without it, "the budget
  kills" and "every task with a budget gets killed" read the same.
- The probe reads the verdicts from the **kernel's audit log** (cursor read,
  DDR-1098). It does not use its own observations, because a probe can fake
  those and cannot fake a kernel-written audit record (DDR-1066).
- Every byte budget is far below the rate limiter's reach (a few syscalls), so
  a rate kill cannot stand in for a budget kill. The rate kill is audited as
  `AR_RATE_KILL` and would fail the exact kind arm.
- Mutants, one per arm, each a separate binary:
  - M1: no timer-path check, so arm T fails.
  - M2: no byte charge in `fd_write_user`, so arm B fails.
  - M3: no action count, so arm A fails.
  - M4: kill every budgeted task, so control arm C fails.

## 6. Not claimed

- No model-token enforcement (§2).
- No CPU-share or per-task memory budget beyond the existing `mem_limit`.
- No nesting.
- Rollback on rejection is Block B item 2, a separate DDR.
- Budgets apply only to tasks started through `SYS_SPAWN_TASK`; the existing
  `SYS_SPAWN_AGENT` path is unbudgeted, as it is today.

## 7. Built and measured

- Shipping kernel `7d429743e211a591`, 1,503,626 B, warning-clean at -Werror.
- `smoke-taskbudget` (shard 7, strict): `T=1/51..53 B=2/4148 A=3/4 C=none`.
  B is exact: a 52-byte start line plus four 1 KiB writes; the fifth write is
  the one charged past 4096.
- **Two charge points the design missed, found by the first run** (B read
  `2/5120`): musl's two-iovec `writev` takes `sys_writev`'s console gather and
  never reaches `fd_write_user`, and io_uring `OP_WRITE` likewise. Both now
  charge. Without that, a task could bypass the byte budget by choice of call.
- Mutants, each a separate binary, each failing exactly its own arm:

  | Mutant | Kernel | Result |
  |---|---|---|
  | M1 no timer-path time check | ccfa2287eaf9d269 | rc=2, `T=none` |
  | M2 no `fd_write_user` byte charge | 534a33ef4a5d827e | rc=2, `B=none` |
  | M3 action charge of 0 (both submit paths) | dce2692b91b60b2c | rc=2, `A=none`, T/B/C correct |
  | M4 kill every budgeted task | 1d32d99506e72f00 | rc=2, `C=2/52` |

- Revert rebuilds to `7d429743e211a591`.
- Not claimed: U1/U2/U3, OPEN-1, OPEN-2 untouched.
