# DDR-970 — CodeRabbit review on PR #5: a stranded console lock, an ungated fs thread, and what was NOT fixed

Status: ACCEPTED. Written before the code it governs (§NON-NEGOTIABLES 5).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

CodeRabbit reviewed PR #5 at `b0c7c20` and posted 16 actionable comments with
**Merge Risk: High**. Findings from a review bot are bug reports: each is
verified against current code here, valid ones are fixed, and the ones that are
declined say why. Two are real defects **introduced by this branch**.

## 1. `g_line_lock` is stranded forever by a ring-0 fault (Major — real)

DDR-963 added `g_line_lock` and holds it across the ~20 `kputs` calls of a
logical line. Two paths take it **blocking**: the `[hb]` heartbeat
(`idt.c:177-271`, every timer tick) and the four `[smp]` announce sites
(`smp.c:68-74`).

The ring-3 trap printer is already safe — DDR-963 §5 gave it
`console_line_trylock()` and releases before `sched_exit()`. **The ring-0 path
is not.** A kernel fault (`vector < 32`, `(cs & 3) == 0`) skips that branch
entirely, falls into the panic block, and ends:

```c
kputs("halting.\r\n");
for (;;)
    __asm__ volatile("cli; hlt");
```

If the faulting CPU held `g_line_lock`, it halts holding it. Every other CPU's
next `console_line_lock()` is `spin_lock_irqsave` — it spins **forever, with
interrupts already masked**. The panic stops being a diagnosable halt and
becomes a whole-machine hang with no output from the other CPUs.

**This is a hazard the branch widened, not one it invented.** `g_console_lock`
has always had a narrower version of it (a fault inside `kputs` strands it too).
What DDR-963 changed is the window: from one `kputs` call to a ~20-call line
held on every timer tick. That is a large enough change in exposure to count as
this PR's defect.

### Decision — force-release at the top of the panic block

`console_line_force_release()` clears `g_line_lock` unconditionally (plain
`spin_unlock`, no flag restore — the panic wants interrupts to stay masked), and
the panic block calls it before printing anything.

Releasing a lock the caller may not hold is normally indefensible. It is correct
here precisely because the path is terminal: nothing after it depends on the
lock's integrity, the lock protects **cosmetic line atomicity only**, and a
panic outranks cosmetics. The alternative failure — other CPUs silently wedged —
destroys the diagnostic value of the panic itself.

Rejected: **stopping the other CPUs with an IPI.** It is the more complete
answer and CodeRabbit suggests it, but it is a new cross-CPU mechanism on the
panic path, in a PR that is not about panics, and it cannot be tested by any
existing gate. Force-release removes the hang with three lines. The IPI halt
belongs with the `#MC` handler work in GROUP A, and is noted there.

## 2. Capability-mint failure still starts `fs_test_thread` (real)

`main.c:2509-2512`, added by DDR-964:

```c
if (fc == CAP_NULL)              /* table full: every FS op would be -EPERM */
    kputs("[fs] FAILED to mint fs capability\r\n");
fst->arg = (void *)(uintptr_t)fc;
sched_unblock(fst);
```

The failure is reported and then ignored — the thread is unblocked anyway.

**The consequence is worse than the comment claims.** "Every FS op would be
`-EPERM`" is true of the *VFS* path, but `fs_test_thread`'s destructive block
calls `sfs_format(sbd)` directly on a `struct blk_device *`, which is not
capability-gated. So a thread that failed to get its capability would still
**reformat the disk** while doing nothing else useful.

This is my own omission and it is inconsistent with this file: the other three
DDR-964 conversion sites (`main.c:211`, `273`, `344`) all do
`sched_destroy(...)` on the error path, commented *"never ran: still BLOCKED"*.
The `fst` site simply missed it.

### Decision
Destroy the still-blocked TCB and do not unblock. Matches the established
pattern in the same file.

## 3. Unbounded `PRADYOS_CAD_ADV` output (real, small)

`compositor.c:773` says *"Four lines per run at most"* and then prints
unconditionally on every advance. On a 180 s `smoke-cadence` run advances
continue past 4, so the comment is false and the serial log grows without bound.
Gate the `printf` on `g_cad_advances <= 4` so the code matches its comment.

## 4. Linker-symbol pointer subtraction — fix the one line this PR adds, not all 30

`main.c:1980` computes `renametest_elf_end - renametest_elf`. Both are
`extern const unsigned char []` linker symbols, i.e. distinct array objects, so
the subtraction is undefined under C11 6.5.6. Cppcheck is right.

But this is the **house idiom**: `grep -c "_elf_end - " kernel/main.c` returns
**30**, all pre-existing, all identical. CodeRabbit flagged only line 1980
because it is the only one inside this PR's diff.

**Decision: fix line 1980, leave the other 29.** This PR introduced one new
instance of the pattern and should not; it did not introduce the other 29 and
should not silently rewrite 29 untouched lines to say so. The sweep is a
mechanical change that deserves its own commit and its own gate run. Recorded
here so the next session finds it rather than rediscovering it.

## 5. Declined, with reasons

- **"Narrow the atomicity claim" (`smp.c:40-42`)** — accepted, it is accurate:
  the trap printer's `console_line_trylock()` really can splice an `[smp]` line.
  Comment corrected.
- **Docstring coverage 35% vs 80% threshold** — declined. The threshold is a
  CodeRabbit default, not a rule of this repo, and this codebase documents with
  block comments carrying measurements and DDR references rather than
  per-function docstrings. Adding 26 stub docstrings would lower the signal.
- **Markdown fence languages, `umounted`→`unmounted`, stale DDR-963 status,
  DDR-960 LBA endpoint wording, DDR-960 per-run vs total, BUILD_TRACKER OPEN-10,
  SESSION_HANDOFF FSRM status** — all accepted and applied. They are cheap and
  several are genuine staleness introduced when the fixes landed after the docs.

## 6. Verification bar

The two real defects are on failure paths that no gate exercises — a kernel
panic, and a full capability table. A green suite therefore proves **no
regression**, not that either fix works, and that limit is the point of stating
it. What is checked directly:

- the panic-path release is verified by reading the emitted call order, and the
  `sched_destroy` path by matching the three sibling sites;
- `smoke-cadence` must still emit `PRADYOS_CAD_ADV` and `PRADYOS_CADENCE_OK`
  (the bound must not suppress the sentinel);
- `smoke-shell` 5/5, `smoke-fsrm`, `smoke-aether-sec`, and the §HYGIENE set.

## 7. What would refute this

- A panic that still hangs the other CPUs → something else on that path holds a
  lock, and the IPI halt is required after all.
- `smoke-cadence` losing `PRADYOS_CADENCE_OK` → the four-line bound was applied
  to the wrong statement.
