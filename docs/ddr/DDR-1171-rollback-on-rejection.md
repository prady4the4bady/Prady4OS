# DDR-1171: Rollback on rejection (stop and undo a task's filesystem effects)

Status: design, committed before code (§NON-NEGOTIABLE 5). Issue #45 Block B item 2.

## 1. What exists today

- `SYS_REJECT_ACTION` (sovereign only) marks a pending action `AE_REJECTED`
  and audits `AR_REJECT`. The agent that proposed it keeps running, and
  every effect it already produced stays.
- SFS has a per-**mount** transaction (`txn_begin/commit/abort`, a root swap).
  It cannot serve as a per-task undo: it is global to the mount, so one
  task's abort would discard every other writer's work on the same volume.
- Nothing records what a task changed, so nothing could undo it.

## 2. Scope, stated honestly

"Undo the project's effects" is only possible for effects the kernel can
record **and** reverse. This DDR covers filesystem effects through the
syscall layer and refuses, rather than tracks, those it cannot reverse:

| Effect (rollback task) | Handling | Why |
|---|---|---|
| create a new file (`open` + `O_CREAT`, file absent) | **logged**; undo = unlink | reversible |
| open an existing file for writing, backend has `truncate` | **logged** with size-at-open, once per path; undo = truncate to that size | SFS writes are append-only (DDR-1089), so the original prefix is intact |
| open an existing file for writing, backend lacks `truncate` (FAT32, ext4) | **refused** `-EPERM` | the append could not be undone |
| `O_TRUNC` on an existing file | **refused** `-EPERM` | data destroyed, not recoverable |
| `unlink`, `rename`, `ftruncate` | **refused** `-EPERM` | data destroyed or the undo needs state not kept |
| log full (16 entries) | further create/writable-open **refused** `-ENOSPC` | an untracked effect is worse than a refused one |
| network egress, IPC sends, actions already approved | **not undone**, stated | bytes that left the machine cannot be recalled |

A rollback task is therefore a task whose filesystem effects are reversible
by construction, not one that is undone on a best-effort basis.

## 3. Interface

- `struct task_budget._pad` becomes `flags`. `TASK_FLAG_ROLLBACK = 1`.
  Every existing caller passes zero (measured: `user/budgettest.c` is the only
  caller of NSI 107), so the field's meaning is unchanged for them.
- The log is a kernel page attached to the TCB (`rb_log`, NULL unless the
  flag is set), initialised in `sched_create_state` (§NON-NEGOTIABLE 10),
  allocated when the budget is armed (before the first run), freed in
  `sched_free_tcb`. `fork` from a rollback task is **refused** (`-EPERM`): a
  child would hold the same root mount without the log, so a fork would be an
  escape from it.

## 4. Trigger and replay

- `sys_reject_action`, after `aether_approve(id, 0)` succeeds, looks up the
  proposing pid. If that task is live and carries a rollback log, it sets
  `rb_pending` and raises `SIGKILL` on it.
- The replay runs **inside the dying task's own `sched_exit`**, beside the
  existing surface/socket reaps. That context has the task's own capability and
  root mount, and it is the one point where the task can no longer write, so
  replay cannot race the task's own writes. Entries are undone in reverse
  order.
- Audited once as `AR_ROLLBACK` (appended, = 30) with
  `action_id = (undone << 32) | failed`. A non-zero `failed` is reported, not
  hidden.
- A task that exits normally before any rejection commits its effects. That is
  deliberate: rejection is of the project still in flight.

## 5. Gate: `smoke-rollback` (new, strict)

The probe is `rbtest`, sovereign, root FAT32. It first creates `/RBOLD.TXT`
with 10 bytes, then spawns `agent_base` tasks with task strings.

`rb-work` does the following in order:

- create `/RBN<pid>.TXT` with 12 bytes;
- try `unlink /RBOLD.TXT` and print the rc;
- try a writable open of `/RBOLD.TXT` and print the rc;
- submit a force-pending `DELETE_FILE`, then spin in ring 3.

Arms (exact values):

| Arm | Expect |
|---|---|
| U | rollback task: `unlink` rc = -1 (`-EPERM`), `/RBOLD.TXT` still opens with size 10 |
| W | rollback task: writable open of existing FAT file rc = -1 (refused, no truncate on FAT) |
| R | after the probe rejects the pending action: task gone, `AR_ROLLBACK` for its pid with undone=1 failed=0, `/RBN<pid>.TXT` absent |
| C (control) | same task string without the flag, same rejection: no `AR_ROLLBACK`, its file still present |

The control separates "rejection undoes the task's effects" from "rejection
deletes files", which would otherwise read the same.

Mutants:

| Mutant | Change | Arm that must fail |
|---|---|---|
| M1 | no replay in `sched_exit` | R |
| M2 | no unlink refusal | U |
| M3 | no create logging | R (`undone=0`, file present) |
| M4 | replay for every rejected task | C |

## 6. Not claimed

- The truncate-undo path cannot be exercised by this gate: the agent's root
  mount is FAT32, which has no truncate. It is refused there, and that refusal
  is gated (arm W). The SFS undo path is built but not gated, and is stated so.
- No undo of network, IPC or approved actions.
- No rollback after the task has already exited.
- Does not close U1/U2/U3, OPEN-1 or OPEN-2.
