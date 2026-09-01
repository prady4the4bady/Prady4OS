# DDR-1034 — `ACTION_RUN_EXPERIMENT`: a bounded sandboxed executor and a separate results store

**Status:** DESIGN (implementation follows in the same branch)
**Date:** 2026-09-01
**Supersedes:** DDR-1021's deferral of `ACTION_RUN_EXPERIMENT`
**Instruction:** operator, PR #17 comment, item 2

---

## §1 — What DDR-1021 measured, and what changes

DDR-1021 deferred this type on three findings, all re-measured today and all
still true of the tree as it stands:

1. `CAP_EXEC` (`cap.h:62`, bit 20) is a `#define` **checked nowhere** — the only
   other occurrence in the kernel is inside a comment at `sched.h:122`.
2. There is **no experiment subsystem** of any kind.
3. The DDR-812 metric lockbox is `CAP_SOVEREIGN` read-only **by design**, so the
   agent being measured cannot write its own result.

DDR-1021 concluded it was `ACTION_EXEC_CODE`'s deferral under another name. That
conclusion rested on an assumption this DDR rejects: **that "run an experiment"
requires a general interpreter.** It does not. What it requires is *something
the kernel runs on the agent's behalf, whose outcome the agent cannot forge.*
Finding (3) is the real constraint, and it is a constraint on the **results
store**, not on the executor.

So the design splits along exactly that line:

- an executor narrow enough that a human gate on it would be theatre (§3), and
- a results store whose write path is kernel-only, achieving the lockbox's
  guarantee **without touching the lockbox** (§5).

## §2 — The two layers of authority

Following DDR-1033, and its lesson, exactly:

| Layer | What it is | Where |
|---|---|---|
| `is_exec` on `struct tcb` | may this process use the door at all. Kernel-set at spawn, **never mintable from ring 3**. | `sched.h`, zeroed in `sched_create` per §NON-NEGOTIABLE 10 |
| `CAP_EXEC` capability handle | a `RES_EXEC` handle minted beside it, checked by `cap_authorize` | `exec_grant()` |

**DDR-1033's lesson is load-bearing here and the fixture is built for it.** Two
independent checks in series each mask the other's absence, and a fixture that
trips both at once cannot tell you which is load-bearing — DDR-1033's arm B was
passing for the wrong reason for exactly that reason, and only a mutant found it.
So the deny process in this design is spawned with `exec_grant()` **and then has
`is_exec` cleared**: it holds the capability and lacks only the door. A mutant
that deletes the `is_exec` check must therefore turn its `-EPERM` into a `0`.

`CAP_EXEC` becomes a real, checked bit as a consequence. That is the operator's
"not a decorative define", and it is satisfied by a `cap_authorize` call on a
path a gate exercises, not by a grep hit.

## §3 — The executor: a bounded integer stack machine

**Scoped as narrowly as it can be while still being an executor rather than a
stub.** The narrowness is the security argument, so each bound is listed with
what it forecloses.

| Bound | Value | Forecloses |
|---|---|---|
| **No memory opcodes at all** | there is no `LOAD`, no `STORE`, no addressing mode | *structurally* — not by a runtime check that could be dropped. The machine cannot name an address, so "no memory access outside its own stack" is a property of the instruction set, not of a guard. This is the single most important line in the design. |
| No syscall/IO opcode | — | any effect outside the syscall's own stack frame |
| No `DIV` | omitted | division by zero, which on x86 is `#DE` and fatal in ring 0. Omitting it is cheaper and stronger than checking it. |
| `EXP_MAX_STEPS` | 4096 | unbounded loops. Returns `-ELOOP`. |
| `EXP_STACK_N` | 32 | operand-stack overflow (`-EOVERFLOW`) and underflow (`-EINVAL`) |
| `EXP_MAX_CODE` | 256 instructions | an unbounded copyin |
| opcode validation | reject unknown | executing whatever an out-of-range byte would index |

Opcodes: `HALT, PUSH, ADD, SUB, MUL, DUP, DROP, SWAP, JNZ`.

**`JNZ` is in the set on purpose, and it is the only opcode that needed
justifying.** Without a branch, every program is straight-line, the step count
never exceeds the program length, and `EXP_MAX_STEPS` becomes unreachable —
a bound whose only reachable value is the passing one. That is the dead-arm
class, seven instances deep in this project, and it would have been the eighth.
With `JNZ`, the step cap is reachable, so the gate can assert on it and a mutant
can defeat it. **The branch exists so the bound is measurable.**

All arithmetic is on `int64_t` and wraps. Wrapping is defined behaviour here
because the operands are `uint64_t` internally and reinterpreted; there is no
signed-overflow UB on any path.

## §4 — Why this is NOT force-pending

`aether_action_forces_pending()` covers `SPAWN_PROCESS`, `DELETE_FILE`,
`REWRITE_AGENT_CODE`, `EVOLVE_GENOME`. `RUN_EXPERIMENT` is deliberately **not**
added, and the reason is stated rather than left implied:

**The human gate exists for actions whose effect is outside the sandbox.** Each
of those four changes something the operator would want to see first — a new
process, a deleted file, rewritten code, a mutated genome. An approved
`RUN_EXPERIMENT` cannot do any of that. Its worst case is consuming
`EXP_MAX_STEPS` of CPU inside one syscall, which is bounded and no worse than
the same arithmetic loop written in the agent's own address space — which needs
no approval at all.

**CORRECTION to this section's first draft, which said "bounded, preemptible".
It is bounded; it is NOT preemptible.** `syscall_init` does
`wrmsr(MSR_SFMASK, 0x200)` (`syscall.c:279`), so **`RFLAGS.IF` is clear for the
whole of a syscall** — the DDR-981 mechanism. The executor therefore runs with
interrupts masked, and `EXP_MAX_STEPS` is not merely an anti-hang convenience:
**it is the only thing bounding a ring-3-supplied loop running with interrupts
off.** Without it, an agent submitting `PUSH 1; JNZ -11` wedges the CPU exactly
as DDR-981 described, which is what M2 measures rather than assumes.

That makes the size of the bound a real quantity, not a round number. 4096 steps
of integer ops is the interrupt-off window this design accepts; the evidence that
it is tolerable is that arm C runs the full 4096 every clean boot and no gate has
produced an `[apfreeze]` or a missed tick from it. That is a measurement of *this*
budget, not a licence to raise it — **raising `EXP_MAX_STEPS` lengthens an
interrupt-off window and must be re-measured, not assumed.**

Adding a human gate to an action that cannot escape its own stack frame would be
security theatre, and it would also make the probe harder for no gain. If the
sandbox ever grows an effect — memory access, I/O, a longer step budget — **this
decision must be revisited in the same commit.** Recorded here so that is not
lost.

## §5 — The results store, and why it does not touch the lockbox

The operator's constraint, verbatim: the results store must not touch the
sovereign read-only metric lockbox, because *"that lock is deliberate (an agent
must never grade itself) and must not be weakened."*

**The lockbox is not touched, extended, or read on this path.** A separate,
clearly-labelled kernel-owned ring:

```c
typedef struct {
    uint64_t seq;        /* monotonic; 0 = empty slot            */
    uint32_t pid;        /* who submitted                        */
    int32_t  status;     /* 0 = HALT reached, else the errno     */
    int64_t  value;      /* top of stack at HALT (status 0 only) */
    uint32_t steps;      /* instructions retired                 */
    uint32_t code_len;   /* program length as submitted          */
} exp_result_t;
```

**The guarantee this reproduces, and how.** The lockbox's property is *the
measured cannot write the measurement*. Here the same property holds by a
different mechanism: **the only writer is the executor**, which runs in the
kernel. An agent submits a program; the kernel runs it and records what
happened. There is no syscall that writes a result, so an agent cannot record a
value it did not compute, cannot overwrite another agent's record, and cannot
delete one. It grades nothing.

**Two limits, stated rather than implied:**

1. **Read is not privileged, and the store is not secret.** `SYS_EXP_RESULT`
   requires `is_exec || is_sovereign` and returns any record by index. The
   guarantee is *write* integrity, not read privacy. If per-agent result privacy
   is ever wanted, that is a separate change.
2. **The ring wraps.** `EXP_RESULTS_N` slots; the oldest record is overwritten.
   `seq` is monotonic and never reused, so a reader can always tell a wrap from
   a gap — but a record that has wrapped is gone. This is a log, not an audit
   chain; the tamper-evident ledger (F#76, `smoke-auditchain`) is the thing that
   is not allowed to lose entries, and this is not that.

## §6 — Syscalls

| NSI | Name | Signature |
|---|---|---|
| 100 | `SYS_RUN_EXPERIMENT` | `(const uint8_t *code, uint32_t len, int64_t *out)` -> `0` \| `-EPERM` \| `-EINVAL` \| `-EFAULT` \| `-ELOOP` \| `-EOVERFLOW` |
| 101 | `SYS_EXP_RESULT` | `(uint32_t idx, exp_result_t *out)` -> `0` \| `-EPERM` \| `-EINVAL` \| `-ENOENT` \| `-EFAULT` |

Next free NSI was 100 (max shipped 99, `SYS_IPC_RECV`, DDR-1033); table size 128.

`ACTION_RUN_EXPERIMENT == 11` gains a `_Static_assert` **because a probe now
hand-copies it.** `aether.h`'s existing comment says 11 is deliberately unpinned
*"because nothing copies it yet and a pin whose probe does not exist reads as a
claim that one does"* — that comment must be updated in the same commit, or it
becomes false.

## §7 — The gate: `smoke-runexp`, and the mutants that must fail it

Probe `user/exptest.c`. **The probe REPORTS and the gate JUDGES** — no `fail()`
before a print, per the rule the dead-arm class produced.

| Arm | What it exercises | Sentinel |
|---|---|---|
| **A** | a real computation runs: `PUSH 6, PUSH 7, MUL, HALT` | `PRADYOS_EXP_CALC rc=0 v=42` |
| **B** | the door: the deny process **holds `CAP_EXEC` and lacks only `is_exec`** | `PRADYOS_EXP_GATE rc=-1` (allow side prints `rc=0`) |
| **C** | the step cap is reachable and enforced (a `JNZ` loop) | `PRADYOS_EXP_LOOP rc=-40` |
| **D** | operand-stack overflow is refused | `PRADYOS_EXP_OVF rc=-75` |
| **E** | the results store recorded what the KERNEL computed, not what the agent said | `PRADYOS_EXP_REC st=0 v=42 steps=4` |

| Mutant | Change | Predicted |
|---|---|---|
| **M1** | delete the `is_exec` check | fails **B** — the deny process holds the capability, so `cap_authorize` cannot save the arm |
| **M2** | delete the step cap | fails **C** |
| **M3** | delete the stack-bound check | fails **D** |

Each mutant is recorded against its own kernel hash (R1).

## §8 — What this does NOT do

- **The AETHER action path still does not call this.** Same residual DDR-1033
  recorded for `SEND_IPC`: an approved `RUN_EXPERIMENT` has no automatic effect;
  a process calls the syscall itself. Wiring the executor into the action
  dispatcher is a separate change and is **not** claimed here.
- No floating point, no memory, no I/O, no nested programs, no persistence
  across boot.
- `smoke-lockbox` must still pass unchanged. If it does not, this design has
  touched something it promised not to.


---

## §9 — MEASURED

Clean build **`f4724a14578eddc3`**, `make image` rc=0, zero warnings at
`-Werror`, `hygiene_check.sh` all three PASSED, `ci-shard-check` 168 gates /
10 shards / 7 excluded.

```
PRADYOS_EXP_CALC rc=0 v=42        <- allow process
PRADYOS_EXP_GATE rc=0
PRADYOS_EXP_LOOP rc=-40
PRADYOS_EXP_OVF  rc=-75
PRADYOS_EXP_REC  rc=0 st=0 v=42 steps=4
PRADYOS_EXP_OK
PRADYOS_EXP_CALC rc=-1 v=0        <- deny process: holds CAP_EXEC, lacks is_exec
PRADYOS_EXP_GATE rc=-1
```

| mutant | kernel | gate | outcome |
|---|---|---|---|
| clean | `f4724a14578eddc3` | PASS | all five arms |
| **M1** drop the `is_exec` check | `8200fd7a8c5f6d9e` | **FAIL** | `required pattern 'PRADYOS_EXP_GATE rc=-1' not found` — the deny process printed `rc=0 v=42`, i.e. it ran the program. Arm B is a test of the door, exactly as intended. |
| **M2** drop the step cap | `b5fef6dda491b787` | **FAIL** | see below — **not** what §7 predicted |
| **M3** drop the stack bound | `7014d721be9a7971` | **FAIL** | `required pattern 'PRADYOS_EXP_OVF rc=-75' not found` — arm D printed `rc=-22` (`-EINVAL`). Every other arm still passed, so M3 lands on exactly one arm. See §9.2: the errno is the least of it. |

### §9.1 — M2 did NOT fail arm C, and what it did instead is the stronger result

§7 predicted M2 would fail arm C by returning something other than `-ELOOP`.
**It never returned at all.** The M2 capture's last line of the entire boot is:

```
PRADYOS_EXP_CALC rc=0 v=42
PRADYOS_EXP_GATE rc=0
```

and then nothing — no arm C, no arm D, no deny process, no further probes, no
end-of-boot sentinel, for the remaining ~110 s until `timeout` killed QEMU. The
gate failed on the **first** missing pattern in its list, which is arm B's
`rc=-1`, not arm C's.

**That is §4's correction demonstrated rather than argued.** `MSR_SFMASK` clears
`RFLAGS.IF` for the whole syscall (`syscall.c:279`), so the unbounded `JNZ` loop
runs with interrupts masked: the CPU cannot take a timer tick, cannot be
preempted, and cannot reach any other thread — including the deny process, which
is why arm B is what goes missing. `EXP_MAX_STEPS` is not an anti-hang
convenience. It is the only thing between a ring-3-supplied loop and a wedged
CPU, and it is the DDR-981 mechanism reachable from an ordinary agent action.

**A second observation, recorded because it is a limit and not a result:** the
M2 capture contains **zero `[apfreeze]` lines**. That detector is printed by the
heartbeat on a CPU that is still running; with the only CPU wedged, nothing can
print, including the thing that exists to name this failure. So a missing step
cap produces **total silence, not a diagnosable error** — the same shape as
OPEN-1 route 1. Do not read "no `[apfreeze]`" here as "no freeze".

**The prediction in §7 was wrong and is left in place above rather than
rewritten**, so the record shows what was expected against what was measured.
The mutant still does its job — the gate rejects it — but it rejects it for a
different and more serious reason than the one designed for.

### §9.2 — M3: the operand-stack bound is not input validation, it is a kernel stack bounds check

M3 fails arm D as designed, and the *shape* of the failure is worth stating
plainly because the arm's name understates what the bound does.

`st` is `int64_t st[EXP_STACK_N]` — **a local array on the kernel stack**, and
`sp` is advanced by a program supplied from ring 3. With the `DUP` bound
deleted, arm D's 40 `DUP`s walk `sp` from 1 to 41 and write `st[32]`..`st[40]`
**past the end of that array, on the kernel stack, with attacker-chosen
repetition count.** The observed `-EINVAL` is not the check working by another
route; it is what happened to be returned after the overflow had already
scribbled over adjacent kernel stack, and it should be read as an accident of
this particular layout, not as a graceful degradation.

So `if (sp >= EXP_STACK_N)` is the only thing between a ring-3 program and a
kernel stack smash. Two consequences, recorded rather than left implicit:

1. **The bound is checked on every opcode that grows the stack** — `PUSH` and
   `DUP` — and a future opcode that pushes must add it too. There is no single
   choke point, because the machine's growth sites are per-opcode.
2. **The arm's sentinel asserts the errno, which is a proxy.** A gate cannot
   observe "did not overflow the kernel stack" directly; it observes "returned
   `-EOVERFLOW` instead of running on". That is a real check and an indirect
   one, and it is written down as indirect here rather than dressed up as a
   memory-safety proof.

### §9.3 — Summary of what each mutant established

| mutant | designed to show | actually showed |
|---|---|---|
| M1 | arm B tests the door, not the capability | exactly that — the deny process ran the program |
| M2 | the step cap returns `-ELOOP` | **more**: without it the CPU wedges with `IF` clear and the boot goes silent — no arm C, no arm D, no other process, and **no `[apfreeze]`**, because nothing is left running to print it |
| M3 | the stack bound returns `-EOVERFLOW` | **more**: it is the bounds check on a kernel-stack array indexed by ring-3 input |

Two of three mutants demonstrated something stronger than the arm they were
written for. Both surprises point the same way — these bounds are load-bearing
for kernel integrity, not just for tidy return codes.