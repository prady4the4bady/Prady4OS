# DDR-963 — multi-call console lines are not atomic under SMP

Status: ACCEPTED (finding + design). **Not implemented** — this DDR exists to
record measured evidence and a design before any code, per R16.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).

## 1. Observation

While scoring DDR-961's `smoke-smpuser` N=20 run, **11 of 20** serial logs
contained a `[trap]` line spliced by another CPU's output:

```
[trap] user [boot-load] SYSTEST.ELF t=#PF page fault pid=180
```

That is the `WXVIOL.ELF` page-fault line with a `[boot-load]` line written
through the middle of it. Both are expected output; neither is a fault. Every
one of the 20 runs had exactly 2 `[trap]` lines and exactly one intact
`METRIC.ELF` trap, so nothing faulted unexpectedly — the *content* is correct
and only its framing is broken.

## 2. Mechanism — and two things it is NOT

`kputs` **is** cross-CPU safe. `irq_save()` in `console.c` is a *shadowing*
local (`console.c:19`) that calls `spin_lock_irqsave(&g_console_lock)`, added by
ADR-030 stage 1 with the note "call sites unchanged". So one `kputs` of one
string emits as one locked unit, and `kwrite` documents itself the same way:
"n bytes as a single locked unit".

The defect is that a **logical line** is often not one call. `idt.c:355-363`
builds the trap line from ~10 separate calls:

```c
kputs("[trap] user ");
kputs(name);
kputs(" pid=");
kputdec(current_thread ? current_thread->pid : 0);
kputs(" name=");
...
```

The console lock is acquired and **released between each call**, so any other
CPU may print in the gaps. Per-call atomicity holds; per-line atomicity does not.

**Not a missing lock in `console.c`** — the lock is there and works.
**Not a `kputs` bug** — each call does exactly what it promises.

## 3. The kernel already solved this — in one file, privately

`kernel/apic/smp.c:40` defines `static spinlock_t g_announce_lock`, and holds it
across the multi-call announce lines:

```c
spin_lock(&g_announce_lock);
kputs("[smp] cpu ");
kputdec(idx);
kputs(tr == ... ? " tss OK\r\n" : " tss FAIL\r\n");
spin_unlock(&g_announce_lock);
```

It is taken at exactly **4 sites, all inside `smp.c`** (lines 66, 82, 90, 102).
Because it is `static`, no other translation unit can take it.

So this is not an unrecognised problem — it is a **solved problem applied in one
place**. `idt.c`'s trap printer, the `[hb]` heartbeat printer and the
`[boot-load]` stamps take nothing.

Note the protection is also partial where it *is* used: `g_announce_lock`
excludes other **announcers**, not arbitrary printers. An `[smp] cpu N tss OK`
line can still be spliced by a heartbeat, because the heartbeat does not take
that lock.

## 4. Exposure — narrower than "every gate", and measured rather than asserted

An earlier note in this session (commits `0a9b312`, `9e0ee66`, and the PR body)
claimed *"every gate here asserts on serial patterns, so any gate matching a
whole line is intermittently exposed"*. **That overstates it, and the correction
matters:**

`boot_test.sh:520` matches with `grep -qF "$pat" "$SERIAL_LOG"` — a **literal
substring search over the whole file**, not a line match. A splice therefore
breaks a gate only when it lands **inside the sentinel's own text**. Sentinels
are short relative to total output, so the per-run probability is far below the
11/20 rate at which splices occur *somewhere*.

The genuinely exposed set is bounded by two conditions — the gate must run
multi-CPU, and its required sentinel must be assembled from multiple calls:

| condition | population |
|---|---|
| gates with `QEMU_SMP=4` | **20** |
| of those, requiring kernel-side `[...]` sentinels | 18 |
| ring-3 sentinels (musl `printf` + `fflush` → one `kwrite`) | **immune** |
| `[smp] …` announce sentinels | partly protected (§3) |

`smoke-user` requires the literal `[trap] user #PF page fault` — exactly the
string observed spliced — but runs single-CPU, so it cannot be hit. That is luck
rather than design: nothing prevents a future gate from requiring a multi-call
sentinel under SMP.

**No gate failure in this session has been traced to this.** `smoke-cadence`'s
CI red was checked against it explicitly and **ruled out** — its sentinel is a
single buffered `printf` + `fflush`. This is recorded as a latent hazard with a
demonstrated mechanism, not as the cause of any known intermittent.

## 5. Design — promote the existing lock, do not invent a new one

The earlier note also described the fix as *"holding the console lock across a
whole logical line, a change to the console hot path and its lock ordering"*.
Having read `smp.c`, that too is an overstatement. The smaller change is:

1. Promote `g_announce_lock` out of `smp.c` into `console.h` as a public
   line-granularity lock (`console_line_lock()` / `console_line_unlock()`),
   keeping `g_console_lock` exactly as it is.
2. Take it around the multi-call printers that emit a line a gate might match —
   starting with `idt.c:355`'s trap line.

`g_console_lock` is a leaf taken *inside* each `kputs`, so a line lock wrapping
several `kputs` calls nests strictly outside it: lock order
`line → console → klog`, acquired in that order at every site, with no path
that takes them in reverse. No change to `kputc`, the UART busy-wait, or the
`klog_lock` nesting the file already documents.

**Deliberately not done in this session.** It is a new cross-file lock in the
fault path, it needs its own N-run verification that no line is spliced, and
nothing currently failing depends on it. §4 is the reason it is not urgent.

## 6. Verification this will require

- `smoke-smpuser` N=20 with a splice detector: **zero** `[trap]` lines
  containing a second `[` tag. Today that count is 11/20 — a denominator that
  makes the fix falsifiable rather than a matter of opinion.
- The §3 partial case must improve too: an `[smp] …` announce line spliced by a
  heartbeat should also go to zero, which is the test that the lock was promoted
  rather than merely reused inside `smp.c`.
- No regression in `smoke-smp`, `smoke-smplock`, `smoke-percpu`, `smoke-swapgs`,
  `smoke-smpjob` — the gates whose sentinels the announce lock already guards.
