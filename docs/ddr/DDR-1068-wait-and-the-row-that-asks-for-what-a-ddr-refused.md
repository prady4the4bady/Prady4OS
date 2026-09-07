# DDR-1068 — PRISM `wait`, and a backlog row that asks for what an earlier DDR deliberately refused

**Status:** DESIGN (§NON-NEGOTIABLE 5 — written before the code).
**Baseline:** `kernel.bin` `cc8135a9463eefed`, 1,286,538 B.
**Backlog row:** Group D, *"B#12 PRISM job control | `$?`/SIGPIPE ✅ — remaining:
full job control, `&`, `wait`, `fg`/`bg`"*.

---

## 1. THE ROW IS WRONG IN BOTH DIRECTIONS

Measured against the tree, not read off the row:

| the row lists as remaining | actual state |
|---|---|
| `&` | **built** (DDR-881) and **gated** — `smoke-shell` drives `run /EXECTEST.ELF &` twice |
| `fg` | **built** (DDR-881) and **gated** — `fg %1` |
| `jobs` | (not listed) **built** and gated, twice |
| `kill %n` | (not listed) **built** (DDR-755) and gated — `kill %99` |
| **`bg`** | **DELIBERATELY REFUSED by DDR-881**, reason in the source — see §2 |
| **`wait`** | **genuinely missing** — no `strcmp(cmd, "wait")` anywhere |

`smoke-jobctl`, the gate name the row carries, **does not exist**; the coverage
is inside `smoke-shell`. That is the DDR-1063 §7c class again — **seventh
instance**, after `smoke-wx`, `smoke-mc`, `smoke-lazystack`, `smoke-vdso-read`,
`smoke-maximize` and `smoke-pipes`.

## 2. `bg` IS NOT UNBUILT WORK — IT IS A RECORDED REFUSAL, AND THAT IS THE WORSE DEFECT

`user/prism.c:253`, DDR-881's own scope statement, written *before* that code and
still in the file:

> **SCOPE, stated before the code.** This is `&`, a job table, `jobs`, `fg`, and
> signalling a job by %n. It is **NOT** ^Z/SIGTSTP stop-and-continue, and it is
> not POSIX job control in the full sense — that needs process GROUPS and a tty
> layer that owns a foreground group, and PRADYOS has neither. **Claiming `bg`
> and `%1` suspension on a kernel with no setpgid and no controlling terminal
> would be a shell pretending to a capability the system does not have.**

Confirmed in the tree rather than taken from that comment:

- `grep -rn "SIGTSTP\|SIGCONT\|setpgid\|tcsetpgrp"` over `kernel/` and `user/`
  returns **only that comment and one other comment** — no implementation.
- `kernel/proc/signal.h` defines **four** signals: `SIGKILL`, `SIGUSR1`,
  `SIGPIPE`, `SIGTERM`. There is no stop signal to send and none to resume from.

**A row that lists a deliberate refusal as remaining work is worse than one that
lists something already built.** A stale "unbuilt" row costs a session the time
to discover the work is done. This one invites a session to *build the thing a
prior DDR refused on stated grounds* — and to ship, in the process, exactly the
pretence DDR-881 named: a `bg` that returns success while nothing resumes,
because there is nothing suspended and no way to suspend it. The row is corrected
to say **refused, with the reason and the missing subsystem named**, so the next
reader inherits the decision rather than re-deriving it or overturning it by
accident.

## 3. `wait` — AND WHY THE OBVIOUS GATE IS VACUOUS

`wait` blocks until every background job has finished. The implementation is
small: `fg` already shows the pattern (`prism.c:793`) —

```c
nsi(SYS_WAIT4, j->pid, (long)&st, 0);   /* blocking: it is fg now */
```

— so `wait` is that loop over the live entries of the job table.

**The obvious gate does not work, and this was measured rather than discovered
after writing it.** The natural arm is:

```
run /EXECTEST.ELF &
wait
echo MARKER
```

…asserting that the probe's output precedes `MARKER`. **That arm is one-sided.**
With `wait` the ordering is guaranteed; *without* it the ordering is merely
*likely* — the probe may still finish first — so the arm cannot separate "`wait`
worked" from "the child happened to be fast". A gate that passes on a shell with
no `wait` at all is the dead-arm class, and this project has now hit it fourteen
times.

**Reporting a count does not rescue it either.** A `wait` that prints
`reaped=N` looks like the quantity that discriminates — but `jobs_reap()` runs at
**every prompt** (`prism.c`, the loop head), and `/EXECTEST.ELF` exits almost
immediately, so by the time the `wait` line is typed the job is already reaped
and a **correct** `wait` reports `reaped=0`. The arm would assert a value meaning
*"there was nothing to wait for"*, which is exactly the state a missing `wait`
also produces.

### 3.1 What a live gate actually requires

**A background job that is still running when `wait` executes.** No such thing
exists in the tree: every probe on the FAT volume completes in milliseconds.
So the gate needs a **duration-controlled** background probe — one that spins a
known wall interval (via `SYS_CLOCK`, whose one-second resolution is adequate
here), prints a marker, and exits. Then:

- **positive:** the probe's marker precedes the post-`wait` marker — guaranteed
  by `wait`, not by luck, because the probe outlives the shell's next input line
  by a stated margin;
- **negative (M1, `wait` removed):** the post-`wait` marker precedes the probe's,
  because the shell does not block.

Both directions, per DDR-1039's rule, and the margin is a design parameter rather
than a hope.

## 4. SCOPE OF THIS CHANGE

1. `user/slowtest.c` — the duration-controlled probe (§3.1).
2. `wait` in `prism.c`, bounded (§NON-NEGOTIABLE / S2: the loop is over a fixed
   job table and each `SYS_WAIT4` targets a pid that exists, so it terminates).
3. Two `smoke-shell` arms, ordering-based, both directions.
4. The Group D row corrected on all six points in §1.

**NOT in scope:** `bg`, for the reason in §2 — it stays refused, and the refusal
is now written where the backlog can see it rather than only in a source comment.

---

## 5. MEASURED — AND M1 CAUGHT A VACUOUS ARM IN THIS DDR'S OWN GATE

`kernel.bin` `cc8135a9463eefed` → **`b68e241eaaa7b03b`**, **1,286,538 B
unchanged** (`slowtest.elf` lives on the FAT volume, not embedded, so it costs
the kernel image nothing and the size/headroom pair is unaffected).
`ci-probe-rodata-check` 75 → **76 ELFs**, none writable-allocated.

### 5.1 The first gate was wrong, and the mutant is what said so

The first version put the injector's **6-second** sleep *after* the `wait` line.
M1 (`wait` disabled) then produced:

```
907: PRADYOS_SLOW_DONE waited=4
909: WAITMARK-7q4
```

**The ordering arm PASSED on the mutant.** The reason is the arm's own harness:
the probe runs 4 s and the injector slept 6 s, so the *injector* did the waiting
and the marker followed the job's line whether or not the shell blocked at all.
That is the DDR-1063 §7c/dead-arm class **inside the gate this DDR is writing** —
and it is exactly the trap §3 was written to avoid, reproduced one level up.
Only the `reaped=1` arm failed, so a single-arm version of this gate would have
shipped a passing ordering check that proved nothing.

**Fix:** the sleep after `wait` is **1 s**, not 6. The shell must now do the
blocking — `wait` holds the queued next line in the serial buffer past the
probe's exit — and the injector cannot substitute for it.

### 5.2 Both directions, re-measured after the correction

| tree | `kernel.bin` | rc | capture |
|---|---|---|---|
| **fixed** | `b68e241eaaa7b03b` | **0 — PASS** | 904 `PRADYOS_SLOW_DONE waited=4` · 906 `PRADYOS_WAIT_OK reaped=1` · 907 `WAITMARK-7q4` |
| **M1** (`wait` disabled) | `cc8135a9463eefed` | **2 — FAIL** | **902 `WAITMARK-7q4`** · **904 `PRADYOS_SLOW_DONE waited=4`** — the ordering is **inverted** |

`waited=4` confirms the probe really did run its full interval rather than
returning early, so the margin the arm depends on is measured, not assumed.
Reverting M1 returns `b68e241eaaa7b03b` **bit-for-bit**.

**A hash coincidence worth reading:** M1's kernel is `cc8135a9463eefed`, which is
*exactly* the DDR-1067 kernel. That is not luck — disabling `wait` returns PRISM
to its pre-DDR-1068 form, and `slowtest.elf` is not embedded, so the whole
`cc8135a9` → `b68e241e` delta is the `wait` builtin and nothing else. It also
confirms the new probe adds zero bytes to the kernel image.

### 5.3 Regression — 8 of 8, hash-verified

On `b68e241eaaa7b03b`, checked before and after: `smoke-shell`,
`smoke-ctrlaltt`, `smoke-iso-userspace`, `smoke-selftest`, `smoke-execve-argv`,
`smoke-fs`, `smoke-blkmq`, `smoke-rqstress-liveness` — all rc=0,
`kernel_after == kernel`. `smoke-ctrlaltt` and `smoke-iso-userspace` are in the
set for the DDR-1067 reason: they run PRISM over a pipe pair in a terminal window
and from the shipped ISO respectively, so the builtin dispatch change is
exercised by two consumers that are not the serial shell.

## 6. NOT CLAIMED

- **`bg` is not built and is not going to be**, per §2 — the refusal is DDR-881's
  and the missing subsystem is named. This change makes that visible in the
  backlog; it does not revisit the decision.
- **This is not POSIX job control.** No process groups, no controlling terminal,
  no `SIGTSTP`/`SIGCONT`. `wait` here waits on the shell's own background
  children and nothing else.
- **`wait` takes no arguments.** POSIX `wait %n` / `wait <pid>` is not
  implemented; the loop is over the whole table. Stated rather than implied.
- **No kernel change.** `SYS_WAIT4` already existed and `fg` already used it.
- **No new gate** — the arms are on `smoke-shell`, per DDR-1039's reasoning.
  Gate count stays **177**.
- **`reaped=` is not the discriminator** (§3) — it is deterministic *here* only
  because `/SLOWTEST.ELF` is still alive, and it would read 0 for a correct
  `wait` in any other arrangement. The ordering arm is what carries the claim.
