# DDR-1009 — the release-candidate kernel fails 1 CI suite in 4, and one failure names a mechanism

**Status:** MEASUREMENT + FIX + DETECTOR GAP. The measurement is the important
part and stands on its own; the fix is a real defect found by reading the one
readable capture, and is **not** claimed to explain all four signatures.

---

## 1. One kernel binary, twelve CI suites, three failures

Every commit from `d0a85b5` through `93a4a1f` on `dev/phase1-seyp3n`, and
`fa29506` on `dev/phase1`, changes **Markdown only**. Verified two ways, because
this whole section depends on it:

```
$ git diff --name-only d0a85b5 93a4a1f
CLAUDE.md
SESSION_HANDOFF.md
docs/ddr/DDR-1005-vdso-callable-reader-assessment.md
docs/ddr/DDR-1006-open2-reopens-sched-tick-site.md
docs/ddr/DDR-999-multiarch-parity-assessment.md

$ git diff --name-only d0a85b5 fa29506
SESSION_HANDOFF.md
docs/ddr/DDR-999-multiarch-parity-assessment.md
```

No source file, no Makefile, no gate script, no shard table. And the kernel was
**rebuilt independently from `93a4a1f` in a clean worktree** during this session
and reproduced `bb9c6187a30bb0dd` bit-for-bit. So the following rows are twelve
independent CI suite-runs of **the same kernel binary**:

| tip | runs | green | failed |
|---|---|---|---|
| `d0a85b5` | push, pull_request, workflow_dispatch | 3 | 0 |
| `4b0b542` | push, pull_request, workflow_dispatch | 3 | 0 |
| `81274f4` | push | 0 | **1** |
| `93a4a1f` | push, pull_request | 1 | **1** |
| `fa29506` | push, workflow_dispatch ×2 | 2 | **1** (DDR-1006) |
| | **12** | **9** | **3** |

Every row was read from the GitHub Actions API for the branch, not carried over
from another document — including `fa29506`'s three runs, which DDR-1006 also
lists and which were re-queried here rather than copied (`33279970481` push
green, `33279992304` dispatch green, `33281593947` dispatch failed).

**A 25% per-suite failure rate**, and the three failures are at **four different
gates with four different signatures** (one run failed two shards):

| run | shard | gate | signature |
|---|---|---|---|
| `81274f4` push | 5 | `smoke-smpuser` | timeout, gate 1 of 14, no panic |
| `81274f4` push | 6 | `smoke-msixap` | **panic banner, no body, total silence to timeout** |
| `93a4a1f` push | 3 | `smoke-nethammer` | timeout at gate 20 of 20, heartbeat alive to t=23500 |
| `fa29506` dispatch | 4 | `smoke-smppreempt` | `[apfreeze]` cpu=2 (DDR-1006) |

### 1.1 What this does to §NON-NEGOTIABLE 1

"3 consecutive greens on the SAME tip SHA" is the promotion rule. At a 25%
per-suite failure rate, three greens in a row happens **0.75³ ≈ 42%** of the
time by chance. And it did: **this kernel has already passed that criterion
twice**, at `d0a85b5` and again at `4b0b542`, 3/3 each.

So the rule is doing less than it looks like it is doing. It is not wrong — it
caught DDR-1006 on `fa29506`, and DDR-1006 rightly says "this is the rule
earning its keep" — but on this evidence **passing it is not evidence the kernel
is sound.** Three greens on one SHA bounds the failure rate loosely; twelve runs
across SHAs that share a binary bound it much better, and that is the number the
release decision should use.

This is a statement about the *evidence*, not a proposal to change the rule.

## 2. The one readable capture, and the mechanism it names

`81274f4`, shard 6, `smoke-msixap`. Last guest lines, verbatim:

```
SYSOPEN OK
SYSFSTAT OK

*** NEXUS KERNEL PANIC ***
[boot_test] FAIL — capture kept: .../serial-5778.log.fail-5778
qemu-system-x86_64: terminating on signal 15 from pid 5785 (timeout)
```

Two things are load-bearing here.

**(a) The panic printed its banner and NOTHING ELSE.** `idt.c:701` emits the
banner; `idt.c:702` is the very next statement, `kputs("component: NEXUS isr")`.
That line never appeared — and neither did anything else, for the rest of the
gate's window, until `timeout` killed QEMU. This is not §INV.23 interleaving:
interleaving garbles lines, it does not produce ~100 s of total silence. **The
machine hung between two consecutive `kputs` calls.**

**(b) The stopping point is `SYSFSTAT OK`.** That is OPEN-1 route 1's recorded
signature exactly — `NEXT_TASK_QUEUE` describes route 1's only prior capture as
hanging "at `SYSFSTAT OK` → `SYSREAD OK`, i.e. inside `sys_read`/`vfs_read`".
Same stopping point, different gate (`smoke-msixap`, not `smoke-surfdestroy`).

### 2.1 The mechanism: a force-release with a sibling nobody force-releases

DDR-970 identified precisely this failure mode and fixed it for one lock
(`console.c:42`):

> "A ring-0 fault taken inside a line-locked region skips the ring-3 trylock
> branch and halts in `cli; hlt` still holding `g_line_lock`, so every other
> CPU's next `console_line_lock()` spins forever with interrupts already masked
> — **a diagnosable panic becomes a silent whole-machine hang**."

That is a description of this artefact. But the fix releases only `g_line_lock`:

```c
void console_line_force_release(void) {
    spin_unlock(&g_line_lock);
}
```

`kputs` does **not** take `g_line_lock`. It takes `g_console_lock` — a
*different* lock (`console.c:18` vs `:21`, "Distinct from g_console_lock and
always taken OUTSIDE it") — for the whole string:

```c
void kputs(const char *s) {
    uint64_t fl = irq_save();          /* spin_lock_irqsave(&g_console_lock) */
    ...
    irq_restore(fl);
}
```

**Nothing force-releases `g_console_lock` on the panic path.** And DDR-979's
one-winner latch made that worse rather than better: a CPU that loses the latch
now does

```c
for (;;) __asm__ volatile("cli; hlt");        /* idt.c:697 */
```

— halting forever, interrupts masked, **still holding whatever it held**. If it
was faulted out of a `kputs`, that includes `g_console_lock`. Before the latch it
would at least have continued into the printer.

That yields exactly the observed sequence: the winner prints the banner
(acquiring and releasing `g_console_lock` cleanly), another CPU takes
`g_console_lock` and then faults and halts holding it, and the winner's next
`kputs` spins on it forever with interrupts already masked. Silent whole-machine
hang, after the banner, with no further byte.

### 2.2 One hypothesis this capture RULES OUT

`kputc`'s UART wait is the other candidate for "hangs mid-print", and it is
**not** it: the THRE spin is bounded (`CONSOLE_THRE_MAX`, then `g_thre_drops++`
and return, `console.c:131-134`). It cannot hang. Checked rather than assumed —
an unbounded `while ((inb(COM1+5) & 0x20) == 0)` was the obvious first guess.

## 3. The fix

Extend the force-release to the lock `kputs` actually holds, and rename it so it
stops advertising a narrower job than it does:

```c
void console_panic_force_release(void) {
    spin_unlock(&g_line_lock);
    spin_unlock(&g_console_lock);
}
```

It is called at `idt.c:673`, **before** the DDR-979 latch, so one edit covers
both the winner (its own printing can proceed) and the loser (it halts having
released both).

**The cost, stated rather than discovered:** releasing `g_console_lock` while
another CPU is legitimately mid-print lets that print interleave with the panic
dump. DDR-970 accepted exactly this trade for `g_line_lock` — "the path is
terminal, nothing after it depends on the lock's integrity, and the lock protects
cosmetic line atomicity only" — and the argument transfers unchanged. §INV.23
already instructs every reader to reconstruct panic fields **by name, never by
line position**, precisely because of it. A garbled dump is strictly better than
no dump and a hung machine.

## 4. A detector gap, independent of all of the above

```
$ grep -c . <(GLOBAL_FORBIDDEN)      # 70 entries, healthy per §NON-NEGOTIABLE 6
$ grep -rn "NEXUS KERNEL PANIC" Makefile tools/ kernel/
kernel/idt.c:701:    kputs("\r\n*** NEXUS KERNEL PANIC ***\r\n");
```

**One emitter, zero consumers.** `*** NEXUS KERNEL PANIC ***` is not in
`GLOBAL_FORBIDDEN` and no gate greps for it. A ring-0 panic is therefore caught
only when it *happens* to break a gate's own assertion or run out its clock — as
it did here. A panic on a boot that had already printed its sentinel, or on a
gate whose assertion the panic does not disturb, **passes**.

That is the same hole DDR-981 recorded for `[vblk] compl wait timeout`: a
diagnostic sitting in serial logs that nothing asserted on, for a long time. The
remedy is the same one: add the sentinel. Verified safe before adding — no gate
in the tree expects a kernel panic, and the string appears zero times in
locally-green captures.

## 5. What is NOT claimed

- **The fix is not claimed to explain the other three signatures.**
  `smoke-smpuser`, `smoke-nethammer` and DDR-1006's `[apfreeze]` produced no
  panic banner at all, so the console-lock path is not implicated in them by
  anything but proximity. Attributing them to it because it is the defect in
  hand is the post-hoc reasoning DDR-1006 §6 refused and this project has had to
  retract before (DDR-966, DDR-969, DDR-973, DDR-975 §7).
- **Nor is it claimed the four are four separate defects.** Twelve runs and four
  signatures do not settle that either way.
- **OPEN-1 route 1 is not closed.** This capture *advances* it — it is the
  second occurrence of that stopping point, on a different gate, and it shows
  route 1 is **not** always silent: this one panicked first. The framing "a hang
  prints nothing, so no panic-based detector can address route 1" (DDR-994) is
  too strong as stated. But a stopping point is not a cause.
- **The rate is a rate, not a diagnosis.** 3 of 12 with a 95% binomial interval
  of roughly 5%–57% — wide. It is nonetheless the best-powered number this
  project has on the release candidate, because it pools runs across SHAs that
  provably share one binary instead of treating each SHA separately.

## 6. What this means for the release (PR #17 STEP 2 / STEP 3)

STEP 3 asks for 3 independent greens on one tip before promoting and tagging.
§1.1 shows this kernel can produce that run of three while failing a quarter of
its suites. The honest reading for the release notes is therefore **not** "CI is
green" but: *the candidate fails roughly one CI suite in four, at four distinct
gates, and three of those four have no named cause.* Whether that ships is the
operator's call, not this file's — but it must be the stated basis for it.

---

## 7. MEASURED

Kernel **`29c792a8b8f3b056`**, warning-clean at `-Werror`.

### 7.1 The lock fix

`console_line_force_release` → `console_panic_force_release`, releasing
`g_line_lock` **and** `g_console_lock`, called at `idt.c:673` — before DDR-979's
latch, so one edit covers both the winner and the loser.

Gates on the new kernel: `smoke-selftest` PASS, `smoke-shell` PASS,
`smoke-blkmq` PASS, `smoke-blk-integrity` PASS, `smoke-rqstress-liveness` PASS,
`smoke-wmmin` PASS, `smoke-perrestore` PASS; `ci-shard-check` OK,
`ci-probe-rodata-check` OK.

**What that does NOT show.** These prove the change is not a regression. They do
**not** show the fix works, because none of them panics — the defect it addresses
is reachable only from a ring-0 fault, which no gate produces on purpose.
Confirmation can only come from the next CI panic printing a *complete* dump
where this one printed a banner and stopped. Stated here so a run of green gates
is not later read as evidence the mechanism was proven.

### 7.2 The detector — verified non-vacuous, both directions

Adding a pattern to a list proves nothing; the list has been silently empty
before (§NON-NEGOTIABLE 6). So the matcher was run against a synthetic capture
and against a real green one:

```
synthetic log containing "*** NEXUS KERNEL PANIC ***"
  -> MATCHED: NEXUS KERNEL PANIC        (detector fires)
a green smoke-shell capture
  -> no pattern matched                 (no false positive)
```

`GLOBAL_FORBIDDEN` parses to **71** lines with the documented `sed` range, which
is the §NON-NEGOTIABLE 6 check — and that range had to be updated in the same
commit, because it terminates on the **last entry in the list** and appending
moved it. A stale terminator makes `sed` emit nothing, the count reads 0, and the
check silently reports the exact catastrophe it exists to detect. CLAUDE.md now
says so explicitly at the invariant.

---

## 8. The measurement is about `bb9c6187a30bb0dd`, NOT about the current tip

Worth stating plainly, because §1's table is the kind of number that gets quoted
without its subject.

`37d220a` (DDR-1007) and `462b713` (DDR-1008/1009) change **code**, so each is a
different kernel — `92eb02028af0a929` and `29c792a8b8f3b056`. The twelve-run
denominator does not transfer to them, in either direction:

- It is **not** evidence the current tip is bad. Nothing measured here ran on it.
- It is **not** discharged by the current tip going green. `37d220a` is 2/2
  (push + pull_request), and §1.1's whole point is that a short run of greens is
  weak evidence at this failure rate — `0.75² ≈ 56%` even if the rate were
  unchanged.

Neither DDR-1007 nor DDR-1008 touches the scheduler, SMP, the block layer or
lwIP, which is where three of the four signatures live, so there is no reason to
expect the rate to have moved. **DDR-1009's fix is the one change that could
plausibly affect one of them** — the `smoke-msixap` hang — and §7.1 already
records that no gate can confirm it.

So the release decision needs its own evidence on whatever tip is actually
promoted, pooled the same way: count suite-runs across every SHA that shares that
kernel binary, not greens on one SHA.
