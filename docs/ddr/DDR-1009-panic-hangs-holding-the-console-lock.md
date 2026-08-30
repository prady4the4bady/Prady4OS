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

### 8.1 Running tally on the successor kernel `29c792a8b8f3b056`

Pooling by the same rule, `462b713` → `9c172c8` → `d7d2794` all carry the DDR-1008
/ DDR-1009 kernel — the diffs between them are Markdown plus, at `d7d2794`,
`tools/qemu_runner/boot_test.sh`, which changes the *harness* and not the binary:

| tip | runs | green | failed |
|---|---|---|---|
| `462b713` | push, pull_request | 2 | 0 |
| `9c172c8` | push, pull_request | 2 | 0 |
| `d7d2794` | push, pull_request | 2 | 0 |
| | **6** | **6** | **0** |

The last two ran under a **strictly stricter** sentinel list than the first four
(`d7d2794` added `NEXUS KERNEL PANIC` and the two `[percpu]` entries), so pooling
them is conservative rather than generous.

**This is not yet evidence the rate improved.** `0.75⁶ ≈ 0.18`, so six greens in a
row is an unremarkable outcome even if the failure rate were unchanged at 25%.
It is recorded as a tally to be continued, not as a result — which is the same
discipline §1.1 applies to the three-green rule.

### 8.2 The tally continued, and it did not stay clean

The next kernel, `9623c163cd479043` (DDR-1012's horizon bands), spans `6d4db94`
and `63c8ead` — the latter is Markdown-only, verified by `git diff --name-only`:

| tip | runs | green | failed |
|---|---|---|---|
| `6d4db94` | push, pull_request | 2 | 0 |
| `63c8ead` | push **green**, pull_request **FAILED** | 1 | **1** |
| | **4** | **3** | **1** |

**CORRECTION.** This table first read "`63c8ead` | push | 0 | 1", i.e. one run
which failed. Wrong: `63c8ead` had **two** runs, and the **push** one passed —
it is the **pull_request** run that went red. The wake notification carried a
`check_run_id` and a shard number but not the event, and I filled in the event
rather than looking it up. Corrected against the run list.

The error made the case *weaker* than the evidence supports, which is worth
noting: **the same commit, same kernel, went green on one event and red on the
other.** That is an intermittent by definition, and it is a stronger refutation
of "this PR caused it" than the version I first wrote.

**`smoke-nethammer`, shard 3, failed at gate 20 of 20** — the same gate at the
same position as the `93a4a1f` failure in §1's table, on a *different* kernel.

Two things follow, and the second is the one that matters.

1. **It is not this PR's.** `63c8ead` changes two Markdown files; the same kernel
   went green on both runs of the immediately preceding SHA.
2. **The signature outlived the kernel it was first seen on.** §1 recorded it on
   `bb9c6187a30bb0dd`; it now recurs on `9623c163cd479043`, across every change
   this session made. So it is not something DDR-1009's console-lock fix, or
   DDR-1010's probe, touched — consistent with §5's refusal to attribute the
   other three signatures to that fix.

The §8.1 caution was written in advance and is now discharged the way it should
be: six greens *was* an unremarkable run, and the seventh run failed. Nothing
here supports a claim that the failure rate has improved on the successor
kernels.


### 8.3 The other successor kernels, and why these tallies must NOT be pooled

Two more kernels have accumulated runs, both verified single-binary by
`git diff --name-only` (a change to `tools/ci/campaign_chunk.sh` is harness, not
kernel):

| kernel | tips | runs | green | failed |
|---|---|---|---|---|
| `4b3181f13b2d76aa` | `f9bdfeb`, `5595470`, `233c81c` | 6 | 6 | 0 |
| `ba6ac01fe015b2a4` | `483e853`, `72a474a` | in flight | — | — |

So the four successor kernels stand at 6/6, 6/6, 3/4, and pending.

**It is tempting to pool them — 15 runs, 1 failure, ~7% against `bb9c6187a30bb0dd`'s
25% — and that would be the exact error §1 was careful to avoid.** §1's number is
good *because* all twelve runs share one binary; the moment you pool across
different binaries you are no longer measuring a kernel, you are averaging four
of them, and a difference between them is precisely the thing at issue.

Per kernel, none has the runs to say anything: `0.75⁶ ≈ 0.18` means six greens is
unremarkable at the old rate, and 3-of-4 tells you nothing at all. **No claim is
made that the failure rate has improved.** The release decision still needs a
run of suites on the binary actually promoted, counted the way §1 counts.
