# DDR-1055 — A required gate sentinel was assembled from three unlocked console
# calls, and another CPU printed into the middle of it

**Status:** IMPLEMENTED + detector + campaign
**Date:** 2026-09-03
**Branch:** `dev/phase1-seyp3n`
**Supersedes nothing. Corrects:** `docs/PRE_LAUNCH_CHECKLIST.md` §4.13 and my own
PR #17 comment, both of which say the recurring `smoke-nethammer` failure's
"root cause is NOT established". It is established here.

---

## 1. The artefact

`build/gatelogs/nethammer.log.fail-3786`, verbatim:

```
[user] ELF loaded (embedded); net hammer spawned=PRADYOS_SOVEGRESS_AUDITED
```

Every character the kernel meant to print is on the wire. The gate's required
sentinel `net hammer spawned=2/2` is not, because a ring-3 probe's whole line
landed between the kernel's second and third console call.

`PRADYOS_SOVEGRESS_AUDITED` is `user/sovegresstest.c:82`, emitted through
`write(2)` -> `fd_write_user` -> `kwrite`.

Local rate on the pre-fix tree: **1 failure in 3 runs** on an idle machine
(`rc=0`, `rc=0`, `rc=2`). CI: four `smoke-nethammer` failures on shard 3, every
one of them on a commit whose `git diff --name-only` cannot change `kernel.bin`,
each shard reporting `kernel.bin: OK` from DDR-1035's hash assertion — so the
binary was identical across the green and red runs, which is what a timing race
looks like and what a code regression does not.

## 2. Mechanism

`kernel/main.c:1836` (pre-fix):

```c
kputs("[user] ELF loaded (embedded); net hammer spawned=");
kputdec((uint64_t)spawned);
kputs("/2\r\n");
```

`kputs`, `kputdec`, `kputhex` and `kwrite` are each individually atomic — they
hold `g_console_lock` with interrupts masked for their whole argument
(`kernel/console.c`, ADR-030 stage 1). A line built from three of them is three
acquisitions with two gaps, and anything that prints can occupy a gap.

## 3. The part that is worth carrying: the line lock did not cover this, and
##    its own header says it does

DDR-963 §5 introduced `console_line_lock()` for exactly this problem, and
`kernel/console.h` describes it as the answer:

> a LOGICAL line assembled from several of those calls is not [atomic]: another
> CPU's printer can land between them. Hold this across such a line.

It excludes **only other holders of `g_line_lock`**. There were four in the whole
tree. `kwrite` — the ring-3 `write(2)` path, and by a wide margin the busiest
printer in this system — never took it. So the mitigation was in place, was
documented as sufficient, and did not cover the printer that actually caused the
failure. Compare DDR-1046, where `vmm_protect_kernel`'s verdict loop printed
`[wx] kernel W^X OK` on a kernel with writable text: a control that cannot see
the case it exists for reads exactly like a control that works.

## 4. Fix — one `kwrite`, not one more lock

`kline` (`kernel/console.{h,c}`): assemble the line into a 256-byte stack buffer
and emit it with a single `kwrite`.

This is chosen over "take the line lock at the 16 sites" for a specific reason,
not for taste. Locking the composite makes it exclusive against other line-lock
holders; it stays open to any **bare `kputs` from another CPU**, because such a
printer takes `g_console_lock` and never consults `g_line_lock`. Those printers
are everywhere (`[vblk]`, `[sched]`, every driver). One `kwrite`, by contrast,
holds `g_console_lock` for the whole buffer, and **every** printer in the tree
takes that lock — so a `kline` is atomic against all of them. It is a strictly
stronger guarantee, and it needs no new locking discipline at the call site.

### 4.1 What was rejected, and why

- **Make `kwrite` take `g_line_lock`.** Two lines, and it would make the four
  existing line-locked sites mean what their comments claim. Rejected: it puts an
  extra IRQ-masked spinlock acquisition on the hottest output path in the system,
  contending against a lock the timer ISR holds for a whole heartbeat line of
  UART busy-waits. That is the cost DDR-1047 refused for `lock_stat`'s hold-time
  measurement, for the same reason — OPEN-2 is a timing-sensitive AP freeze and
  perturbing this path can move it rather than measure it. `kline` obtains a
  stronger result without touching the path at all.
- **Make every printer line-lock-aware via a per-CPU recursion guard.** Zero call
  site churn and it would fix cosmetic interleaving everywhere. Rejected on the
  identity source: the guard needs a CPU id at every `kputs`, from the first
  print in `stage2` onward. `lapic_id()` is invalid before the LAPIC is mapped,
  and the GS-based per-CPU id is precisely what DDR-1010 caught being *wrong*
  (`[percpu] gs FAIL (syscall ctx)`) — a broken GS would then corrupt the console
  in the exact situation where the console is the only diagnostic left.

### 4.2 Overflow is loud

A silently truncated sentinel is the same class of failure this DDR removes, so
`kline_emit` emits its own `[kline] TRUNC` line on overflow, and that string is
added to `GLOBAL_FORBIDDEN`. `KLINE_MAX` is 256 against a longest current user of
about 60 bytes. Per §NON-NEGOTIABLE 6 the entry was inserted **before** the final
list line, so the documented verification command's `sed` terminator did not have
to move; running CLAUDE.md's command verbatim gives **74 before / 75 after /
not 0**, and CLAUDE.md's stated count is updated in this same commit.

## 5. Scope — enumerated by measurement, not by reading

The 268 `EXTRA_SENTINEL` patterns were extracted from the Makefile and each was
asked one question: **does any single string literal in the tree contain it?**
If yes, one call emits it and it cannot be spliced.

- 186 have a single-literal home. Safe, untouched.
- 82 do not. Of those, most are ring-3 probes, which format into a buffer and
  emit it with one `write()` — one `kwrite`, already atomic. Not at risk.
- **16 are assembled in ring 0 from several calls.** Every one was confirmed
  *not* inside a `console_line_lock` region by a source-order depth scan, not by
  eye.

21 sites were converted in total: those 16, plus the four AP-announce lines in
`smp.c` that *were* line-locked (the lock is removed — it did not exclude
`kwrite`, and one `kwrite` does), plus `smp_test_job`.

`kernel/idt.c:748` is **deliberately left alone**: it uses
`console_line_trylock` and prints anyway on failure, because a trap printer that
blocks turns a diagnosable fault into a hang. Its residual splice risk is a
documented trade, not an oversight.

## 6. Proof

- **M1 is not synthetic — it is the pre-fix tree**, and it is already measured:
  1 failure in 3 local runs, with the spliced line captured verbatim (§1) and
  four CI failures on binary-identical commits. Reproducing it again would spend
  QEMU time to re-derive an artefact already on disk.
- **Campaign on the fixed kernel:** `smoke-nethammer` x20 on one binary
  (`kernel=` recorded before and after, per DDR-1035's discipline). Every capture
  is kept at a per-run path and asserted to contain `[hb]` lines before it is
  scanned — DDR-1023's methodology correction, where a campaign's "clean" scan
  turned out to have run over make output containing no boot log at all.
  Per run the campaign records `rc`, the capture size, the heartbeat count, and
  **both** `sentinel_intact` (`net hammer spawned=2/2`) and `spliced`
  (a `net hammer spawned=` line that is not the intact one), so a pass is
  distinguishable from a run that never got there.

  Results: see §8.

## 7. What is NOT claimed

- **OPEN-1, OPEN-2, OPEN-12 and OPEN-13 are untouched.** This is a console
  atomicity defect. It explains `smoke-nethammer`'s recurring red and the two
  other single failures whose signature is a missing required sentinel on a
  binary-identical commit; it says nothing about an AP freeze, a ring-0 `#PF`, or
  a double free.
- **Not every spliceable line is fixed.** Cosmetic ring-0 lines that no gate
  asserts on were left as they are — a spliced `[sfs]` progress line costs a
  reader a moment; it does not fail a gate. The 268-pattern sweep bounds what was
  examined: gate sentinels. `idt.c:748` is named above as a deliberate exception.
- **`build/gatelogs/nethammer.log.fail-2398`** is a second kept failure capture
  in which every required sentinel is present and intact, no `GLOBAL_FORBIDDEN`
  pattern appears, and `nethammer_check.py` returns 0 — i.e. it satisfies every
  assertion I can apply to it offline. Why that run was recorded as failing is
  **not established**, most likely a different invocation, and it is deliberately
  **not** folded into the 1-in-3 figure rather than assumed to be the same
  defect. Colour-matching two failures because they share a gate name is the
  mistake DDR-980 recorded about OPEN-13.

## 8. Measurements

| what | value |
|---|---|
| kernel (fixed) | `5f0a2f60d56fbd9b` |
| `kernel.bin` | 1,196,426 B against the 1,572,864 B gate |
| build | `make image` rc=0, zero warnings at `-Werror` |
| `GLOBAL_FORBIDDEN` | 74 -> 75, terminator untouched |
| pre-fix local rate | 1 fail / 3 runs, artefact `nethammer.log.fail-3786` |
| campaign | **20/20 PASS**, one binary, `kernel_after == kernel` |
| per-run capture | 38,212-38,717 B, **47 `[hb]` lines each** (non-vacuous) |
| `sentinel_intact` | 20/20 | 
| `spliced` | **0/20** |

`(2/3)^20 = 3.0e-4` against the pre-fix 1-in-3. Even at a conservative 10% the
bound is `0.9^20 = 0.12`, so 20 clean runs is decisive against the observed rate
and merely suggestive against a much rarer one — stated that way rather than as
"the defect is gone".

**Regression — one gate per converted print site, plus the hygiene gates.**
18 of 18 rc=0, `kernel_after == kernel`, zero failures:
`smoke-selftest`, `smoke-shell`, `smoke-blkmq`, `smoke-rqstress-liveness`,
`smoke-blk-integrity`, `smoke-smp`, `smoke-smpjob`, `smoke-user`,
`smoke-wxkernel`, `smoke-numa`, `smoke-numa-alloc`, `smoke-nvme`, `smoke-ahci`,
`smoke-e1000e`, `smoke-net-lo`, `smoke-fs`, `smoke-mkfs-sfs`, `smoke-uefi`.

Every converted site is covered by exactly one of those, which is why the list
is what it is rather than a round number: `smoke-smpjob` carries `[smp] cpu N
job OK`, `smoke-smp` the four AP announces and `cpus online=4/4`, `smoke-user`
`[user] sys_exit(` and `[sfs] lz4+tags`, `smoke-fs` `[rtc] 20`, `smoke-mkfs-sfs`
`[acpi] DSDT _S3_`, `smoke-uefi` `NEXUS: E820 map, entries=`, and so on. A
botched conversion fails its own gate loudly rather than quietly changing a line
nobody reads.

**`smoke-selftest` is the load-bearing one.** It is DDR-791's meta-test for a
silently broken `GLOBAL_FORBIDDEN`, and this change edits that list.

**CI, independently:** both suites on the fix commit `bf784f7` are green
(push run 33779267674, PR run 33779273206), including shard 3 where
`smoke-nethammer` runs. That is two of the three §NON-NEGOTIABLE 1 greens; one
green run is not a rate and is not offered as one.


---

## 9. The same defect exists in RING 3, and it explains a second CI failure

Found while enumerating §5, and named here rather than left for someone to
rediscover. It is **not fixed by this commit.**

A gate asserts in two places: the `EXTRA_SENTINEL` list that `boot_test.sh`
checks, and a **post-check `grep` in the Makefile recipe**, which usually matches
the *whole* measured line. §5's sweep covered the first and not the second, so it
classified `PRADYOS_ACTIONDEL_OK id=` as safe — correctly, because that exact
prefix does sit in one literal. The post-check is stricter:

```make
ln=$(grep -ao "PRADYOS_ACTIONDEL_OK id=[0-9]* st=-*[0-9]* ctrl=[0-9]* keep=[0-9]*" build/actiondel.log | head -1)
test -n "$ln" || { echo "[actiondel] FAIL — no measured line in the capture"; exit 1; }
```

and `user/actiondeltest.c:172` builds that line from nine calls — which is
**eleven `write(2)`s**, because `wrdec` emits one digit per write, so `id=258`
alone costs three (measured in DDR-1056, correcting this paragraph):

```c
wr("PRADYOS_ACTIONDEL_OK id="); wrdec(id); wr(" st="); wrdec(st);
wr(" ctrl="); wrdec(ctrl_gone); wr(" keep="); wrdec(keep_present); wr("\n");
```

Every probe in the tree rolls its own `wr()` as one `SYS_WRITE`, and most carry
the same digit-at-a-time `wrdec`, so this is **eleven** `write(2)`s, eleven
`kwrite`s, eleven separate `g_console_lock` acquisitions and **ten gaps** — the
ring-0 defect exactly, one ring out, and worse per line.

**This is very likely the `smoke-actiondel` failure recorded in
`docs/PRE_LAUNCH_CHECKLIST.md` §4.14** — `[actiondel] FAIL — no measured line in
the capture` on CI shard 1, tip `c8c93ed`, a **docs-only commit** whose
`kernel.bin` was bit-identical to a 32/32-green run an hour earlier, and which
failed on the *same push* as the third `smoke-nethammer` timeout. A boot that
PASSes, a full window consumed, and a whole-line grep that finds nothing is what
a spliced composite looks like from outside.

Stated as **likely, not established**: I have not read that capture's
`PRADYOS_ACTIONDEL_OK` line to confirm a splice is present in it, and until that
is done or the fix is measured, this is a matching mechanism rather than a
proven attribution — which is the distinction DDR-1019 insisted on for
`[apfreeze]`'s three producers.

At least these post-check patterns are whole-line matches over composite ring-3
output: `PRADYOS_ACTIONDEL_OK`, `PRADYOS_ACTIONHYPO_OK`, `PRADYOS_ACTIONQUERY_OK`,
`PRADYOS_ACTIONREAD_OK`, `PRADYOS_ACTIONSPAWN_OK`, `PRADYOS_HORIZON`,
`PRADYOS_TERM_CHORD`, `PRADYOS_WM_MAX`, `PRADYOS_WM_CYCLE`, plus the `[rqfree]`
pair.

The fix is the same shape and belongs in `user/include/pradyos.h`: a line builder
that emits ONE `write(2)`. Deliberately a separate change, with its own gate
evidence, rather than widening a commit whose measurement is already complete.
