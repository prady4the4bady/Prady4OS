# DDR-833 — kernel headers are build prerequisites

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `Makefile` `$(KERNEL_BIN)` prerequisites
**Family:** the recurring structural defect — a build that reports success without doing the work
**Written after the fix** — recorded here because it was found mid-verification of
DDR-832 and fixed in the same commit. The ADR-before-code rule was not followed
for this one; noting that rather than back-dating the decision.

## The defect

```make
$(KERNEL_BIN): $(KERNEL_ASMS) $(KERNEL_CS) $(KERNEL_LD) $(USER_ALL_SRCS) ...
```

`$(KERNEL_CS)` is a list of `.c` files. **No kernel headers appeared anywhere in
the prerequisites.** Editing `kernel/aether/aether.h` therefore left `make image`
printing:

```
make: Nothing to be done for 'image'.
```

and every gate afterwards ran the **previous** binary.

## How it surfaced

While verifying the DDR-832 fix. The three gates that DDR-832 was meant to repair
still failed, and the obvious reading was "the fix does not work". It did work —
it had never been compiled.

Had that reading been accepted, the next step would have been hunting a second,
non-existent bug. That is exactly how both wrong OPEN-11 root causes were
produced: a measurement taken through a broken path, believed.

## This is the third instance in the same rule

The comment directly above the rule already documents it twice:

- **DDR-822** — `user/` sources were not prerequisites; 14 of 31 probes tested stale.
- **DDR-825** — `kernel/crypto/` sources and the Makefile were not prerequisites;
  `make image` reported success while `build/fe25519_user.o` did not exist.

and it ends: *"the wildcard fixed `user/` and stopped there."* It stopped one
directory short a third time.

## Decision

```make
KERNEL_HS := $(wildcard kernel/*.h) $(wildcard kernel/*/*.h) $(wildcard kernel/*/*/*.h)
```

added to `$(KERNEL_BIN)`'s prerequisites.

## Why a wildcard and not per-file dependencies

`clang -MMD` generated depfiles would be more precise. But the recipe compiles
every object in one rule rather than one-object-per-rule, so depfiles have
nothing to attach to, and restructuring the whole build to gain precision is a
larger change than the problem justifies. A wildcard over-rebuilds; it never
under-rebuilds, and under-rebuilding is the failure mode that costs days.

## The rule this earns

**When a prerequisite list is fixed by adding one category, ask what else belongs
in it before declaring it fixed.** Sources and headers are both inputs; a rule
that lists one and omits the other is not "mostly right", it is silently wrong
for exactly the edits that change an interface — which are the edits most likely
to break something far away.
