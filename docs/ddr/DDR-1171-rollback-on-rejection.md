# DDR-1171: Rollback on rejection (stop and undo a task's filesystem effects)

Status: design committed before code (§NON-NEGOTIABLE 5, `8fb277a`); BUILT and gated, with the
design corrected where implementation measured it wrong (§7). Issue #45 Block B item 2.

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

## 7. Built — and what the build corrected

**§2's "writable open" rule could not be keyed on the open flags.** Measured:
`sys_open` stores the flags and never checks the access mode, and neither
`fd_write_user` nor io_uring's `OP_WRITE` consults it, so **every FD_VFS fd is
writable whatever it was opened with**. A rule refusing `O_WRONLY|O_RDWR` opens
would be bypassed by opening read-only and writing anyway. So, for a rollback
task, an existing file it did not itself create gets `FD_RB_RO` on its fd, and
both FD_VFS write paths refuse that fd with `-EPERM`. This drops the
"log size-at-open, undo by truncate" path from §2: **existing files are never
writable by a rollback task on any backend**, so there is nothing to truncate
back and the only logged effect is a create. Simpler and strictly stronger.
The kernel-wide access-mode gap is recorded, **not fixed here** (out of this
DDR's scope; it is a POSIX-conformance change touching every writer).

**Control task changed.** §5 said the control runs the same task string
without the flag. Measured on paper before running: an ordinary task's
`unlink /RBOLD.TXT` really succeeds and destroys the shared fixture R's arm U
reads. The control runs `rb-ctl`, which does the same create and the same
parked DELETE_FILE and skips the two refusal probes.

**Shipped:** `TASK_FLAG_ROLLBACK` (was `_pad`), `AR_ROLLBACK` = 30 (pinned),
one PMM page per rollback task allocated **before** `elf_load_args` (a failed
allocation refuses the spawn; the task never runs unlogged), refusals in
`sys_open` (O_TRUNC on existing, create without log room), `sys_unlink`,
`sys_rename`, `sys_ftruncate`, `sys_fork`, and the two write paths; replay in
the dying task's `sched_exit`; `sched_rollback_pid` sets `rb_pending` and
SIGKILL under the same lock as `sched_signal_user_pid` (DDR-1159).
`kernel.bin` 1,503,626 -> 1,515,914 B (+12,288: the log code + the `rbtest`
probe).

**Gate `smoke-rollback` (shard 1, strict, 150 s)** requires exactly:
`PRADYOS_RBTASK task=rb-work create=12 unlink=-1 wexist=-1`,
`PRADYOS_RBTASK task=rb-ctl create=12`, and
`PRADYOS_ROLLBACK rj=0/0 R=1/0 rnew=gone C=none cnew=kept old=10 end`.
Every value on the last line is read from the kernel (audit record, `open`
rc, `fstat`), not from a task's own report. Clean run: rc=0.

Shipping kernel `55b24ffc5ba14a40`, 1,515,914 B. Mutants, one change each,
each on its own hash, each failing the gate (rc=2) on the field predicted:

| Mutant | Change | Hash | Probe line (failing field in **bold**) |
|---|---|---|---|
| M1 | no replay in `sched_exit` | `2d4ef7118a0d5a3f` | **R=none rnew=kept** |
| M2 | no unlink refusal | `3e9bc99b4f327148` | **unlink=0** wexist=-2 … **old=-1** (fixture destroyed) |
| M3 | no create logging | `7e68190ab406ea1d` | **R=0/0 rnew=kept** |
| M4 | log armed for every budgeted task | `b82547552909ed97` | **C=1/0 cnew=gone** |
| M5 | no `FD_RB_RO` write refusal | `681bcdbacf386293` | **wexist=1** |

**A defect in this DDR's own probe, found by M4 and fixed before shipping.**
M4's first run read `C=1/0 cnew=kept`: the control was rolled back, yet its
file still opened. A kernel trace showed the unlink returned 0, a kernel
reopen failed, and the probe's own `open` of that path returned `-ENOENT`
(`existed=0`) — while the probe printed `fd 0`. The probe's `struct kstat` was
120 bytes; the kernel copies out its full 144-byte `struct stat` (Linux layout,
with `__unused[3]`), so `fstat` overran the frame and zeroed the neighbouring
local holding the open result. In the clean run that local is legitimately
`>= 0`, so the overrun was invisible there — it only showed under the mutant
whose control result had to flip. The struct now carries the tail and a
`_Static_assert(sizeof == 144)`. The other two `fstat` users in `user/`
(`ftrunctest.c`, `systest.asm`) size it at 144 and are unaffected. Re-run: M4
now reads `cnew=gone`, the clean run unchanged.

Not claimed (in addition to §6): the SFS truncate-undo path no longer exists
(see above); rejection is still not an undo of network/IPC/approved actions.
