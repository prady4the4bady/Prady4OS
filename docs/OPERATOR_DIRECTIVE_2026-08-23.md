# PRADYOS — OPERATOR DIRECTIVE — 2026-08-23 (SUPERSEDES ALL PRIOR SCOPE LIMITS)

**Read this file in full before starting any session. This is a binding scope
and deadline change, not a suggestion.**

---

## 1. THE DEADLINE

**New hard deadline: 2026-08-28 23:59 UTC.**

This replaces the 2026-08-24 deadline. It is a real extension, not a soft
target, and it will not move again. `v1.0.0` must be tagged on `main` with the
FULL scope below complete — not a minimal/demo release — by this deadline.

## 2. THE SCOPE CHANGE — NO MORE SILENT DEFERRALS

Prior sessions correctly used the "PRE-APPROVED EXCEPTIONS" table in
CLAUDE.md to defer non-essential items. That table is now **SUSPENDED** for
every item that falls inside the x86_64 v1.0.0 feature set the operator
actually asked for: kernel/backend, userspace, the full AETHER agent roster,
and the full Layer 7 UI/UX design as specified in the project's own design
docs. **Do not silently defer any of the items in section 3 below.** If an
item genuinely cannot ship by the deadline, it must be reported explicitly —
named, with a reason and an estimate — not folded into an exceptions table
and left unmentioned.

The only work still legitimately out of scope for this deadline:
- aarch64 / riscv64 / Apple Silicon full ports (boot-only scope stands)
- Phase 10 (quantum layer) — after v1.0.0 only
- Cloud bridge activation (DDR-793) — explicit security-posture decision, stays deferred
- Rust rewrite — out of scope permanently

Everything else — **all of it** — is in scope for this deadline.

## 3. MANDATORY FULL SCOPE FOR v1.0.0 (x86_64)

This is the complete list. Work through Groups A–H in CLAUDE.md's backlog,
but treat every item below as REQUIRED, not optional, including the ones
previously marked "not started":

### Backend / kernel (Group A, B, C)
- Close B#3 (the CPU-3 AP-liveness freeze, DDR-977) with an actual fix, not
  just a diagnosis. This is the last known correctness blocker. As of
  2026-08-22 the mechanism is known (CPU 3 stops taking its own LAPIC timer
  interrupt early in boot and never resumes) but the cause is not — do not
  patch virtio_blk.c, that subsystem is behaving correctly.
- Demand-paged stack (ALREADY BUILT — ADR-038, do not rebuild), I/O APIC
  migration, SMEP/SMAP, kernel W^X residual, `#MC` handler, KASLR, scheduler
  timed-block, lock contention instrumentation.
- FAT32/SFS storage completeness items (free-tree persistence, GC, quotas,
  NUMA, ext4 write) — all items in Group B's table. NOTE: "FAT32 multi-cluster
  read fix" is CLOSED as refuted (DDR-973) — do not reopen without a fresh
  `FAT32MC FAIL` artefact from the gated smoke-fat32-multicluster test.
- Networking completeness: UDP, socket poll/select, IPv6, TAP loopback, TLS.

### Userspace / shell (Group D)
- `argv`/`envp` marshalling, PRISM readline/line-discipline, pipes,
  redirection, quoting, job control (`&`, `wait`, `fg`/`bg`), scripting.
- `SYS_MPROTECT`, `SYS_POLL`, `SYS_FUTEX`, pthreads, `mmap` file-backed
  mappings, dynamic linking, full `io_uring`, full POSIX signals.
- NOTE: PRISM `run` is NOT disabled and never was (DDR-973). Only
  init-driven fork+execve RESPAWN of PRISM is unbuilt — build that.

### THE FULL UI/UX DESIGN (Group E) — THIS IS NOT OPTIONAL
This is the part the operator specifically flagged as non-negotiable. Build
every item in Group E's table, not a subset:
- Real modifier keys (F-keys, arrows, Alt, Ctrl, Meta/Super) — not just Tab.
- Super+M physical sovereign-mode toggle.
- Alt-Tab with real modifier plumbing. Ctrl+Alt+T launches a PRISM terminal
  window.
- Per-window restore from dock (not just restore-all).
- Window maximize at REAL display size, not capped at 512×512 (see DDR-975
  sec.7 — the client-side resize-ack handling, not the WM, is the actual bug).
- Pointer resize handles on ALL edges, not just bottom-right.
- `SURF_EV_CLOSE` notifications so an app can save state before forced close.
- The OKLab horizon-band / animated mesh background work from DDR-716.
- The vDSO callable clock reader for ring-3 (`vdso_entry.asm`).
- Fix `smoke-surfdestroy`'s intermittent (OPEN-1).
- NOTE: a flat `preempt=` counter during a compositor capture is NOT evidence
  of a scheduler freeze (DDR-975) — the compositor's yield-loop legitimately
  produces that signature. Do not misdiagnose UI bugs as scheduler bugs.

If the design docs in `docs/design/` specify visual details beyond this list,
build those too — this directive does not narrow the design spec, it removes
permission to skip it.

### The full AETHER agent roster (Group F) — ALL of it
Eleven agents are currently "not started." Build all of them:
`architect_agent`, `healer_agent`, `inventor_agent`, `tournament_agent`, the
subconscious world model, `verifier_agent`, the sovereign NL UI, capability
discovery, lineage memory, the tamper-evident ledger, plus wiring
`CAP_OCR`/`CAP_EXEC`/`CAP_SCENE`/`CAP_NET_BROWSE` so PRAX, LUMYN, AHNIS, and
IRIS are actually spawnable. This is the "backend AI layer" the operator
means when they say "everything we planned" — it is not decoration, it is
core scope for this deadline.

### Release (Group H, x86_64 only for this deadline)
- x86_64 ISO fully working — DONE as of PR #6 (DDR-971/972): the ISO now boots
  a real OS with filesystem, PRISM, AETHER, networking and compositor on BOTH
  BIOS and UEFI paths (DDR-978 fixed UEFI's missing ACPI/PCI discovery too).
  Verify these fixes land on `main` via PR #6's merge.
- `prad` package manager (NSI 88–90).
- Full invariant suite S1–S8, including S3/S7 once F#66–72 land.

## 4. FASTER DEVELOPMENT METHODS — SET THESE UP NOW

The prior pace will not close this scope by 08-28. Change HOW work happens,
not just what gets built:

1. **Parallel branches per group.** Open one scratch branch per Group
   (A through H) and work them concurrently instead of strictly serially.
   Merge each into `dev/phase1-seyp3n` the moment its own gates are green,
   rather than batching everything into one giant PR. Smaller, faster PRs.

2. **Risk-tiered verification, not a flat N=20 for everything.** Scheduler,
   SMP, security, and memory-safety changes keep the N=20 rule — that
   discipline is correct and stays. UI/UX visual items, agent-roster
   additions, and userspace features that don't touch the scheduler,
   capability system, or memory model may use N=5–8 with the same
   falsifiable-denominator discipline, since a compositor color bug and a
   race condition do not carry the same cost of being wrong.

3. **Batch DDR-writing.** For a cluster of related, well-understood items
   (e.g. the eight Group E UI items), write all their DDRs in one pass before
   implementing any of them, so implementation isn't interrupted by
   documentation context-switches.

4. **Widen the CI shard matrix** if the current 6 shards are becoming the
   bottleneck as more gates are added (149 and growing) — this is an
   infrastructure change, do it once, early, rather than repeatedly waiting
   on a saturated matrix.

5. **Report progress in numbers, every session, in `SESSION_HANDOFF.md`:**
   items shipped this session / items remaining in each Group / current
   total against the 286-item tracker. The operator should be able to read
   one file and know exactly where things stand, without needing to ask.

6. **No stopping for confirmation.** Per CLAUDE.md's existing autonomous
   operation rules — those stand. Work continuously through this list. Task
   switch on CI-wait, never idle-stop.

## 5. WHAT "DONE" MEANS NOW

Replace CLAUDE.md's existing "WHAT DONE MEANS" checklist with this one for
this deadline:

- [ ] Every item in section 3 above is built, gated, and CI-green
- [ ] B#3 is FIXED (not just diagnosed — diagnosis is done, DDR-977)
- [ ] PR #6 merged, `dev/phase1` promoted to `main`, 3 greens on the final tip
- [ ] x86_64 ISO manually walked through again on the FINAL tip (not the one
      already verified — verify the actual release candidate)
- [ ] `v1.0.0` tagged on `main`
- [ ] `SESSION_HANDOFF.md` shows 0 remaining items in Groups A–H's x86_64 scope,
      or an explicit, named, reasoned exception — not a silent gap

This is the one chance to get this right. Build everything, report honestly,
and do not let the deadline slip a second time.
