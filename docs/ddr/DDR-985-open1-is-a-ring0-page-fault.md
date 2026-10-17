# DDR-985 — OPEN-1 is a ring-0 #PF, not the DDR-981 family

*(The hypothesis this DDR set out to confirm — that OPEN-1 was downstream of
DDR-981 — was REFUTED by its own measurement. The title states the outcome, not
the hypothesis. Sections 1-4 are the reasoning as it stood before the run and are
kept deliberately; §5 is what actually happened.)*

**Status:** COMPLETE. Claim A **REFUTED** by measurement (19/20, sec.5). Claim B stands.
**Outcome:** OPEN-1 stays OPEN, and now has its first local artefact — a ring-0 `#PF`.
**Date:** 2026-08-23
**Relates to:** DDR-775 (the family), DDR-776/777 (instruments), DDR-806 (refuted fix),
DDR-977 (the AP freeze), **DDR-981 (the named mechanism)**.
**Supersedes as an attribution:** the "unknown / open, passive" row for OPEN-1 in
`docs/BUILD_TRACKER.md:113`, `docs/PRADYOS_MASTER_PLAN.md:340`, and CLAUDE.md
§OPEN ISSUES.

## 1. Two claims, one measurement

**Claim A (attribution).** `smoke-surfdestroy` is not a surface-lifecycle defect.
It is one member of a three-gate `-smp 4` family that DDR-775 already identified
as a single defect, and DDR-981 named that defect's mechanism.

**Claim B (bookkeeping).** The label `OPEN-1` points at two different gates in
two different places in this repo, because the family had three members and
successive DDRs each picked a different one as the representative.

Claim B is settled by reading (§3). Claim A is a hypothesis with a named
mechanism and is settled by the measurement in §5 — not before.

## 2. What DDR-775 actually established

DDR-775 grouped three gates, all `-smp 4`, each missing a *different* sentinel:

| gate | what it missed | where it stalled |
|---|---|---|
| `smoke-surfdestroy` | the FIRST sentinel (`..._CHURN_OK`) | after `SYSFSTAT OK`; next is `SYSREAD OK` — inside `sys_read` -> `vfs_read` -> SFS |
| `smoke-blk-integrity` | `[smp] blk integrity OK` | concurrent read data-verify |
| `smoke-smpuser` | `[smp] user on AP OK` | inside `smpuser_proof()`'s deadline poll |

DDR-775's own summary of the pattern: *"The three failures share only `-smp 4`
and miss **different** sentinels each time."* It then reached, as its
**"UNIFYING HYPOTHESIS"**, explanation **(A): under `-smp 4` the timer tick
intermittently stops advancing `g_ticks`** — and noted the systemic consequence,
*"every `g_ticks`-bounded wait in the tree is only as bounded as the timer."*

DDR-777 shipped a discriminator for (A) vs (B) scheduler starvation vs (C)
guard/ordering. DDR-806 proposed a reordering fix, implemented it, and
**refuted and reverted** it the same day.

So the family stood with a correct hypothesis and no mechanism for ~3 weeks.

## 3. The mechanism arrived under a different name (DDR-981)

DDR-981 root-caused B#3/OPEN-2: `SYSCALL` entry clears IF via `MSR_SFMASK`
(`syscall.c:229`) and the entry path never restores it, so **every yield-spin
reachable from ring 3 spun with interrupts masked**. `context_switch` preserves
per-thread RFLAGS, so the mask rides across the switch; two such threads on one
CPU hand off forever and never reach idle's `sti; hlt`. That CPU takes no
further interrupts — including its own LAPIC timer.

That is explanation (A), exactly: a CPU on which `g_ticks` stops advancing.
It predicts, without further assumption, every one of DDR-775's observations:

| DDR-775 observation | DDR-981 predicts it |
|---|---|
| different sentinel missing each time | the freeze lands wherever the boot happened to be |
| the DDR-776 vblk watchdog printed **nothing** | the watchdog is driven from the timer path, on a CPU no longer taking timer interrupts |
| local runs pass | needs two yield-spinning threads co-resident on one CPU — timing-dependent |
| `-smp 4` only, never `-smp 1` | ditto |
| "hang, not slowness", 180 s insufficient | a livelock, not a slow path |

And the surfdestroy member is the most direct hit of all. Its recorded stall
point is inside `sys_read` -> `vfs_read` -> SFS. Two of DDR-981's five fixed
call sites are on exactly that path: `mnt_lock` (`kernel/fs/vfs/vfs.c:27`) and the blocking
console read (`sys_io.c:293`). The gate that looks like a compositor test never
reached any surface code.

## 4. The label drift (Claim B), and why it happened

- `docs/ddr/DDR-806-...md:37` annotates `main.c:1311 smpuser_proof(); ->
  "[smp] user on AP OK"` with `<- OPEN-1`.
- `docs/BUILD_TRACKER.md:113`, `docs/PRADYOS_MASTER_PLAN.md:340`,
  `docs/NEXT_TASK_QUEUE.md:46` and CLAUDE.md §OPEN ISSUES all define OPEN-1 as
  the `smoke-surfdestroy` intermittent.

Both are faithful to DDR-775 — it is one defect with three faces, and each
document picked a different face. The cost is that OPEN-1's "four hypotheses
refuted" / "six hypotheses refuted" history reads, in the current tables, as
investigation history for `smoke-surfdestroy` specifically. It is not: it is the
family's history, and most of its artefacts are `smoke-smpuser` and
`smoke-blk-integrity` captures.

This is the same failure mode as §INV.2 (Items 47/48 conflated), and as the
FAT32 case where ADR-024's hedged "most likely" was copied forward as fact.
The general lesson, stated once: **a label is not a defect.** When one mechanism
has several symptoms, name the mechanism and list the symptoms under it; do not
let a symptom inherit the mechanism's investigation history.

## 5. The measurement REFUTED Claim A

**Protocol as run:** `smoke-surfdestroy` x20 consecutive, local, kernel
`d31b4023b0f74d06` @ `46ece3f` (R1), logs in `build/gatelogs/open1/`.

**Result: 19/20 pass, 1 fail — run 16.** Claim A predicted 20/20. It does not
hold, and is withdrawn.

### What run 16 actually shows

The gate missed `PRADYOS_SURFDESTROY_CHURN_OK` **because the kernel panicked**,
not because a sentinel was late:

```text
[sfs] 64K write/read byte-exact OK
*** NEXUS KERNEL PANIC ***
, grow component: NEXUS isr
exception: to 69632 OK
#PF page fault
```

This is **not** the DDR-981 signature. Across all 20 runs there is no
`[apfreeze]`, no `[vblk] compl wait timeout`, and no missing-sentinel-with-live-
system. DDR-981 fixed a livelock; this is a ring-0 **#PF**.

So the §3 mechanism chain, however tidy, was the wrong explanation for *this*
gate. Recorded as a refutation, not softened: 19/20 is not 20/20, and one
failure with a named exception is worth more than nineteen passes.

### Three findings that are worth more than the closure would have been

**(a) First LOCAL reproduction of an OPEN-12-class panic.** DDR-979 recorded
OPEN-12 as CI-only — "0/10 locally". This is 1/20 locally, on a gate anyone can
run. OPEN-12 has been unreachable for want of a reproducer; it now has one.

**(b) OPEN-1 and OPEN-12 may be one defect.** surfdestroy's intermittent is a
ring-0 panic. **Not claimed as settled:** DDR-775's surfdestroy capture stalled
after `SYSFSTAT OK` with *no* panic, and this one panics much later, after the
SFS 64K test. Different stall points, so these may still be two things. What is
established is that at least one surfdestroy intermittent is a panic.

**(c) DDR-979 §5/§6 is incomplete, and the one-winner latch is not sufficient.**
That section attributed the garbled dump to two concurrent *panics* interleaving
and shipped a one-winner latch. This capture garbles anyway — the interleaving
text is an ordinary `[sfs] …, grow … to 69632 OK` from another CPU, not a second
panic. The cause is DDR-970's `console_line_force_release()` on the panic path,
which deliberately drops the console lock to avoid a machine-wide hang. That
tradeoff is defensible; what is not defensible is believing the dump is now
readable. **It is not.** A second mechanism remains.

Note also the exception type differs from DDR-979's capture (`#GP`, vector
0x0D); this is `#PF`. Either two defects, or one corruption producing varied
faults. Do not assume the latter without evidence.

### My campaign design was flawed — stated so the next run is not wasted

I did not set `KEEP_SERIAL=1`, so `boot_test.sh` used a `mktemp` `SERIAL_LOG`
and deleted it (`boot_test.sh:23,35`). The make-log tail truncates at
`#PF page fault`, so there is **no `vector=`, `RIP=`, or `CR2=`** from run 16 —
the three values that would name the faulting instruction and address.

Next campaign MUST set `KEEP_SERIAL=1`. At the observed 1/20 rate, 20 runs is
~64% to reproduce and 40 runs ~87%.

## 6. Status of the two claims

- **Claim A: REFUTED.** DDR-981 did not make `smoke-surfdestroy` clean. OPEN-1
  stays OPEN, now with its first local artefact.
- **Claim B: STANDS** (settled by reading, §4). The label drift is real and the
  split should still happen — and it matters more now, because OPEN-1's real
  signature turns out to be a panic, which is not what its table row describes.

## 7. What this does NOT claim

- Not claimed: that OPEN-1 *is* OPEN-12. See (b).
- Not claimed: that the panic is an SMP race, a compositor bug, or in SFS. The
  panic follows an SFS line but `component: NEXUS isr` says the fault was taken
  in an interrupt handler; the preceding line is where the boot had got to, not
  where the fault is.
- Not claimed: that 19/20 is an improvement over any prior figure. DDR-775
  recorded `3/3 local PASS` on this gate while the defect was live, so local
  pass counts have no established baseline here (R17).
