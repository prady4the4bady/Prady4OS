# DDR-740 — Context-switch perf: skip FPU save/restore for kernel threads

**Status:** proposed (pre-code)
**Layer:** proc (scheduler hot path). Targets the Layer-2 ≤1500 ns switch goal.

## Problem

`schedule()` unconditionally `fxsave`s the outgoing thread's FPU state and
`fxrstor`s the incoming thread's on **every** switch (sched.c, the eager
per-thread FPU save/restore, ADR-023 §D8). Each is a 512-byte FXSAVE/FXRSTOR.
But the kernel is built `-mgeneral-regs-only` and its string routines use no SSE
(verified: no `xmm`/`movdq`/`movap` in `kernel/string.c` or the arch asm), so
**kernel threads never touch the x87/SSE register file.** Saving and restoring
it across a kernel↔kernel switch is pure waste — and the boot benchmark
(`bench_ctx_switch`: idle ↔ partner, both kernel threads) pays it on every one of
its 200 000 switches. Measured ~1881 ns/switch locally (TCG-inflated cycles;
real-hardware target is ≤1500 ns).

## Decision — save/restore FPU only across user threads

Guard the two calls on `is_user`:

```
if (prev->is_user) fpu_save(prev->fpu_state);      /* a user thread may have dirtied XMM/x87 */
if (next->is_user) fpu_restore(next->fpu_state);   /* only a user thread will read it back  */
```

**Correctness.** The invariant is that a user thread's FPU state is saved when it
last stops running and restored when it next runs, and that no code observes a
stale register file. Trace the transitions:
- **U → K:** save U (it may have dirtied FPU); do not restore K. The physical
  register file still holds U's state, but K is `-mgeneral-regs-only` and never
  reads it.
- **K → V:** do not save K (it never dirtied FPU); restore V. V sees its own
  saved state.
- **U → V (no kernel between):** save U, restore V — unchanged from today.
- First run of a user thread: restored from its clean init template (as before).

So every user thread still sees exactly its own FPU state; kernel threads, which
cannot read the register file, are simply skipped. `smoke-fpu` (two concurrent
ring-3 XMM users) exercises the U↔U path and must stay green — that is the
correctness gate.

The `fpu_state` field, its init template, and fork's FPU inheritance are all
unchanged. This is not lazy-FPU-via-CR0.TS (no #NM handler, no trap) — it is a
static skip keyed on a flag already computed in the switch path, so it adds a
single predictable branch and removes two 512-byte memory ops on the common
(kernel-involved) switch.

## Gate

No new gate. The boot already prints the measured cost
(`NEXUS: context_switch ~N cycles (~M ns)`); the slice is validated by that
number dropping (local before/after) and by **`smoke-fpu` staying green** (FPU
state is still correct across user threads). The ns figure is a real-hardware
target, not CI-assertable (TCG runners have no meaningful cycle timing), so CI's
role here is the correctness regression: `smoke-fpu`, `smoke-user`,
`smoke-cowfork` (fork FPU inheritance), and the SMP set (kernel-thread storms),
then the full suite.

## Non-goals

- No CR0.TS lazy FPU (trap-on-first-use) — more complex, needs an #NM handler;
  the static `is_user` skip captures the same win for kernel switches with none
  of the trap machinery.
- No change to the context_switch asm (already minimal), the FPU init template,
  or fork inheritance.
