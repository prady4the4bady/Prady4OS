# DDR-843 — three standing decisions taken, and a throughput lever refuted by measurement

**Status:** accepted
**Date:** 2026-08-06
**Governs:** Group 1 item 3, `ACTION_BROWSE_WEB`, gate verification cadence
**Authority:** operator instruction to take the recommended decisions

---

## Decision 1 — CMake is SKIPPED for v1.0.0 (Group 1 item 3)

Resolved-by-substitution. The Makefile is sufficient for an x86_64 release:

- One architecture, one toolchain (clang/lld/nasm), no external consumers of the
  build, no IDE integration requirement.
- The defect CMake would plausibly have prevented — stale prerequisites — is
  already closed **structurally** by `KERNEL_ALL_CS`, `KERNEL_HS` and
  `USER_ALL_SRCS`, which cannot go stale the way a hand-maintained list did
  (DDR-822/825/833/835).
- Porting ~2,400 Makefile lines carrying 133 gate recipes would rewrite the exact
  machinery that proves the release, re-opening the class those four DDRs closed.

**Revisit post-1.0**, when `arch/aarch64` and `arch/riscv64` become real: per-arch
toolchain files are where CMake actually pays, and those ports are deferred.

---

## Decision 2 — `ACTION_BROWSE_WEB` is DEFERRED post-1.0

It needs a headless browser **and outbound network egress**, i.e. the cloud
bridge (DDR-793), which the operator's own deferred list marks do-not-touch
"unless something in this queue has a hard dependency on it".

Nothing in the x86_64 release path depends on it: the ISO, the invariant gates,
the boot path and the filesystem work all stand without it. The dependency runs
the other way — `BROWSE_WEB` needs the bridge, not the release.

**This is the one deferral I would not take silently even under a general
mandate.** Enabling it turns on outbound network egress from an agent-capable OS,
which is a security posture change, not a feature toggle. It is logged in the
tracker as deferred with this reason, and it needs an explicit "yes, enable the
cloud bridge" to move — a general "build everything" does not carry that weight.

---

## Decision 3 — the timeout lever is NOT available. Measured, not assumed.

I proposed tuning `TIMEOUT_S` per gate from measured time-to-sentinel, citing the
CI-load brief's own formula ("completes in Xs, timeout set to 2X"). Measuring it
killed the idea:

| gate | time to its probe sentinel (local) | current `TIMEOUT_S` |
|---|---|---|
| `smoke-coderewrite` | 23 s | 120 s |
| `smoke-auditchain` | 24 s | 120 s |
| `smoke-auditchain-tamper` | 23 s | 120 s |

2× observed would be ~48 s, and that is exactly the trap DDR-828 already
documented: **seven red CI runs on 2026-08-03 were a 60 s window that was
generous locally and marginal under CI load**, not a defect. CI runners here have
demonstrably run 2-4x slower than this machine. 24 s local is therefore
plausibly ~96 s in CI, and 120 s is the correct number.

**Conclusion: 120 s stays.** The lever I proposed would have re-created a
failure this project already root-caused and fixed. Recorded so it is not
proposed a third time.

### The 20x cadence also saves less than it appears

The obvious companion lever — 20x only for timing-sensitive gates, fewer runs for
deterministic ones — mostly does not apply here. `smoke-coderewrite` rendezvouses
through agent memory with yield loops; `smoke-auditchain` depends on how many
audit records the boot has produced and whether the ring has wrapped. Both have
genuine timing dependence. Cutting their runs would be cutting exactly the gates
where repeated runs have already caught real bugs.

**Honest throughput conclusion:** ~1.5-2 h per gated feature is close to the
floor under the current rules, and neither lever changes it materially. The
constraint is real, not a matter of effort or batching.

---

## The rule this earns

**Measure a proposed optimisation before adopting it — including your own.** Both
levers looked obviously correct when proposed and one of them would have
re-introduced a bug this project had already fixed. A performance idea is a
hypothesis, and hypotheses in this codebase have a poor record against
measurement.
