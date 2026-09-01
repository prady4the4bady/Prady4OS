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

| Mutant | Change | Must fail |
|---|---|---|
| **M1** | delete the `is_exec` check | **B** — the deny process holds the capability, so `cap_authorize` cannot save the arm |
| **M2** | delete the step cap | **C** — and the gate must time out cleanly rather than hang the kernel; the loop is preemptible |
| **M3** | delete the stack-bound check | **D** |

Each mutant is recorded against its own kernel hash (R1). M2 additionally
demonstrates the bound is the only thing between a ring-3 program and an
unbounded kernel loop, which is the claim §3 makes.

## §8 — What this does NOT do

- **The AETHER action path still does not call this.** Same residual DDR-1033
  recorded for `SEND_IPC`: an approved `RUN_EXPERIMENT` has no automatic effect;
  a process calls the syscall itself. Wiring the executor into the action
  dispatcher is a separate change and is **not** claimed here.
- No floating point, no memory, no I/O, no nested programs, no persistence
  across boot.
- `smoke-lockbox` must still pass unchanged. If it does not, this design has
  touched something it promised not to.
