# DDR-1035 — CI builds the kernel once, and the "one binary" property becomes checked rather than inferred

**Status:** IMPLEMENTED
**Date:** 2026-09-01
**Instruction:** operator, PR #17, item 5 (toolchain caching + shared build artifact)

**On ordering (§NON-NEGOTIABLE 5).** The design for this was written and
committed *before* the code, but as **`docs/PRE_LAUNCH_CHECKLIST.md` §1.5**
(commit `e99be3a`) rather than as a numbered DDR — it was written as the safety
confirmation the operator asked for before implementation. This file is the
formal record and adds the measured evidence. Saying so rather than
back-dating a design doc.

---

## §1 — What was actually happening

Measured from `ci.yml`, not assumed. Each of the **ten** `build-and-boot` shards
independently ran: `apt-get install` of the full toolchain,
`rustup toolchain install nightly` + `rust-src` + the `x86_64-unknown-none`
target, `make toolchain-check`, `make smoke-selftest`, `make musl`, `make lwip`,
`make image`, `make ci-probe-rodata-check`, then its own shard.

So **ten identical kernel compiles per CI run**, plus ten apt installs and ten
Rust toolchain installs.

## §2 — Why sharing the build is safe here

Three facts, each checked:

1. **`$(KERNEL_BIN)` has no per-object prerequisites.** `Makefile:408` lists
   source files plus `$(MUSL_LIB)`, `$(MUSL_CRT)`, `$(LWIP_LIB)` — the entire
   compile is one recipe. So a downloaded build tree whose mtimes are current is
   genuinely up to date as far as make is concerned; there is no missing `.o`
   that would drag a full rebuild back in.
2. **No sharded gate rebuilds the kernel or changes its flags.** The one gate
   that does — `smoke-fs-liveness`, which rebuilds with `BSP_LIVENESS=1` — is
   **already excluded from the matrix for exactly that reason** (DDR-777/790).
3. **Rust has exactly one consumer.** `make toolchain-check` (`Makefile:335-344`)
   links a `no_std` parity ELF. Nothing in `kernel.bin` depends on `RUST_LIB`
   and no `smoke-*` gate uses it. So the shards install **no rustup at all** —
   a removal, not a cache, which is why this DDR adds no `actions/cache` step
   for it.

## §3 — The hazard, and the measurement that shows it is real

**`make` is mtime-driven and `download-artifact` does not preserve mtimes.** A
downloaded `build/kernel.bin` looks *older* than the freshly checked-out
sources, so make rebuilds it — which either silently keeps the cost this change
exists to remove, or, worse, **runs the gates against a different binary than
the build job produced.**

That second outcome is a correctness problem, not a performance one: DDR-1009's
twelve-run table, DDR-1023's 20/20 and DDR-1010's 36/36 all reason from *which
runs shared one binary*.

**Measured locally, both directions** (the mutation-equivalent for a build
change):

| tree state | `make -n image` |
|---|---|
| build tree touched current | `Nothing to be done for 'image'` — and `make -n smoke-runexp` emits **zero** kernel compile/link lines |
| one kernel source touched newer | **253** build lines — a full rebuild |

So the `touch` step is **load-bearing**, not defensive decoration, and the
hazard is reproducible rather than theoretical.

## §4 — The property this makes stronger

Before: *"all ten shards ran the same binary"* was **inferred** from them
compiling the same sources.

After: the build job publishes `sha256sum kernel.bin > kernel.sha256` and every
shard runs `sha256sum -c kernel.sha256` **twice** —

- **before** its gates, which catches a bad or mis-nested download; and
- **after**, with `if: always()`, which catches a rebuild that happened
  underneath the gates. It runs even on a red shard on purpose: *"the gates were
  red"* and *"the gates ran a different binary"* are different findings, and the
  second must not be hidden by the first.

That is a net gain in rigor, not a trade against it.

## §5 — Deliberately unchanged

- **`fail-fast: false`** — one red shard must not hide reds in the others.
- **The 3-independent-greens rule** and the `workflow_dispatch` trigger that
  makes a second green on one SHA possible (§INV.15).
- **Every gate's timeout, sentinels and logic.** Only where the compile happens
  changed.
- **`smoke-selftest` stays per-shard.** It is a setup step in every shard *by
  design* (DDR-785): a shard whose gates trust the boot harness must have
  checked the harness first. Folding it into the build job would break that.
- **The shards' apt package list is otherwise identical.** Trimming it further
  would risk a gate quietly losing a tool, for a saving that caching already
  covers.

## §6 — What is NOT uploaded, and why

Disk images. `fat-image` and `sfs-image` are `.PHONY` (`Makefile:732/786`) and
regenerate per shard regardless, and `$(IMG)` rebuilds from `kernel.bin` with
nasm + dd. Shipping them would add ~185 MB per shard for files that are
overwritten on arrival. The uploaded tree is ~38 MB.

## §7 — What this does not establish

**It has not yet run in CI.** §3's evidence is local: it shows make's behaviour
with and without current mtimes on this Makefile. It does **not** yet show that
`upload-artifact`/`download-artifact` round-trip the tree at the expected path.
If the artifact nests differently than expected, the §4 assertion fails loudly
on the first shard rather than corrupting a result — that failure mode was
chosen deliberately, but it is a prediction until a run confirms it.

Nothing here bears on OPEN-1, OPEN-2, OPEN-12 or OPEN-13.
