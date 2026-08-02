# DDR-822 — the build system had the DDR-791 trap built into it

**Status:** Implemented
**Date:** 2026-08-02
**Found by:** misdiagnosing DDR-820 for an entire session.

## §Problem

`$(KERNEL_BIN)`'s prerequisites were a **hand-maintained list** of
`$(USER_*_SRC)` variables. It named **17 of the 31 files in `user/`**.

Editing any of the other 14 did not rebuild the kernel image. `make image`
reported success, `make smoke-<gate>` ran, and the gate tested **the previous
binary**. No warning, no error — the build was "up to date" because the thing
that changed was not something the Makefile had been told to look at.

The unreferenced set was not a random 14. It included **every crypto probe**:

```
sha256test  hkdftest  lockboxtest  sigpipetest  privacynettest  x25519test
metrictest  sfsroottest  fsrmtest  syscallfuzz  bigwritetest  dmesgtest
killtest  setnametest  sysinfotest  timetest  rtcmonotest
sovegresstest  egressaudittest
```

This is exactly the DDR-791 stale-artefact trap — the one this project already
knows about, has a rule against, and checks for in every A/B — except installed
in the build system itself, where the A/B discipline could not see it.

## §What it cost

DDR-820's entire "cause unknown" section. The `_start` alignment fix
(`force_align_arg_pointer`) was written, believed to be under test, and **was
never in any binary that ran.** Every subsequent observation was made against a
16:19 artefact while the source said 18:18:

- "`#GP` at `rip=0x80000020FD`" — the pre-fix binary, correctly.
- "after the alignment fix it neither traps nor prints" — **the same binary**;
  the difference I attributed to the fix was noise in what I grepped for.
- "still fails at `TIMEOUT_S=300`, so it is stuck not slow" — same binary again.
  A correct measurement of the wrong artefact.

Three independent observations, one stale file, one confidently wrong
conclusion recorded in a DDR and a handoff. The alignment fix was right the
first time.

## §Fix

```make
USER_ALL_SRCS := $(wildcard user/*.c) $(wildcard user/*.h)

$(KERNEL_BIN): ... $(USER_ALL_SRCS) ...
```

A wildcard cannot go stale when a probe is added. The previous design required a
human to remember to extend a list every time a probe was written, and the
failure mode of forgetting was **silent and looked like success** — the worst
possible combination.

The `.h` glob is included deliberately: `x25519.h`, `aead.h` and friends are
edited alongside their probes, and a header-only change that does not rebuild is
the same defect wearing different clothes.

## §Why there is no QEMU gate for this

The defect is that the build system does not rebuild. A boot gate cannot observe
it, because a boot gate runs whatever binary exists and this bug's whole
character is that the binary is plausible-looking and wrong.

What is asserted instead, and it is a real check rather than a shrug:

```
touch user/<any probe>.c ; make image
```

must relink. Verified across the previously-unreferenced set — the arm that
matters is a file from the 14, not one of the 17 that always worked.

## §The general lesson, since this is the second instance this session

DDR-817 found eight gates that were in the Makefile and in **no CI run**,
because `ci.yml` hand-listed 111 steps and nothing compared that list to the
Makefile. DDR-822 finds 14 sources that were in `user/` and in **no dependency
list**, for the same structural reason.

Both are the same bug: **a hand-maintained list that must be kept in sync with a
directory, where drifting silently produces a green result.** `ci.yml` drifted
and CI got faster; the Makefile drifted and builds got faster. Both looked like
success.

The fix in both cases is the same shape — derive the list, and assert the
derivation (`make ci-shard-check` for gates, `$(wildcard)` for sources). Any
remaining hand-maintained list in this repo that mirrors a directory should be
treated as a latent instance of this defect, not as working code.

## §A second host-side defect found while verifying this one — and a likely OPEN-9 answer

While re-running `smoke-x25519` to confirm the fix, five consecutive runs failed
with `kernel sentinel 'NEXUS KERNEL OK' not found` — while plain `make smoke`
passed on the same image seconds earlier. The serial log said why:

```
qemu-system-x86_64: -device virtio-blk-pci,drive=disk0,bootindex=0:
    Failed to get "write" lock
Is another process using the image [build/pradyos.img]?
```

A **leaked `qemu-system-x86_64` process** was still holding the image's write
lock. Every gate started while that process lives fails instantly and reports
the *sentinel* as missing, because QEMU exits before printing anything — the
harness sees an empty serial log and blames the kernel.

**This is a strong candidate for OPEN-9**, the long-standing "`smoke-shell`
fails ~5/5 on the dev host but passes in CI on identical code" defect. Every
recorded symptom matches:

| OPEN-9 symptom | leaked-QEMU explanation |
|---|---|
| fails 5/5 locally, passes in CI | CI gets a fresh runner every job; a dev host accumulates orphans |
| identical binary, opposite verdicts | the binary is irrelevant — QEMU never runs |
| "recovered overnight" with no change | the orphan was eventually reaped |
| tracks the host, not the tree | it is literally a host process |
| twice caused a change to be wrongly blamed | the failure is indistinguishable from a boot failure at the harness level |

It also explains why reverting never helped: the revert was never the variable.

This is recorded as a hypothesis with strong supporting evidence, **not as a
closed defect** — confirming it means catching `smoke-shell` failing while a
stray QEMU is live, which has not yet been observed directly. The cheap
mitigation, which should be its own slice: have `boot_test.sh` detect the lock
error explicitly and report *"host has a stale QEMU holding the image"* rather
than *"kernel sentinel not found"*. A harness that misattributes a host problem
to the kernel is how two sessions were spent blaming the tree.
