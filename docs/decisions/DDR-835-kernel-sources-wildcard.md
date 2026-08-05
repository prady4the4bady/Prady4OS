# DDR-835 — kernel sources are a wildcard, not a hand-maintained list

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `Makefile` `KERNEL_ALL_CS`
**Family:** the recurring structural defect — **fourth instance in the same rule**

## The defect

`KERNEL_CS` is an explicit list of `.c` files. A **newly added** kernel source
gets an explicit compile line in the recipe, so it compiles — but it is not in
the prerequisite list, so editing it triggers no rebuild:

```
make: Nothing to be done for 'image'.
```

The gate then runs the previous binary.

## How it surfaced

Adding `kernel/aether/vault.c`. `smoke-vault` failed with `-EIO`, so I added
per-step diagnostics to `vault_store`, re-ran, and **no diagnostic appeared**.
Then added diagnostics to `vault_load`; still nothing. Three gate runs were spent
reading a binary that had never contained any of the code being debugged.

Worse: the actual bug had **already been fixed** two runs earlier. `vfs_read` and
`vfs_write` return a BYTE COUNT, not 0, and the first version of `vault.c`
treated any non-zero return as failure — so a fully successful 9,728-byte write
was read as an error. That fix was correct and simply never compiled.

## This is the same rule, failing for the fourth time

| DDR | list that was incomplete |
|---|---|
| 822 | `user/` sources |
| 825 | `kernel/crypto/` sources + the Makefile |
| 833 | kernel **headers** |
| **835** | kernel **sources** |

Each fix added the one category that had just bitten, and stopped.

## Decision

```make
KERNEL_ALL_CS := $(wildcard kernel/*.c) $(wildcard kernel/*/*.c) $(wildcard kernel/*/*/*.c)
```

added to `$(KERNEL_BIN)`'s prerequisites alongside `KERNEL_HS`. Between the two
wildcards, every `.c` and `.h` under `kernel/` is now a prerequisite, and adding
a file to the tree can no longer produce a silently stale build.

`KERNEL_CS` is kept because the link line and compile rules still reference it;
it now controls *what is compiled*, while the wildcards control *when*.

## The rule this earns

**A hand-maintained list of inputs is a staleness bug with a delay fuse.** The
fix for a stale list is not a longer list — it is removing the human from the
loop. Four separate DDRs were spent adding one category at a time to the same
rule; the wildcard ends the series.

## Cost

Wildcards over-rebuild: touching any kernel file rebuilds everything. That is
roughly 40 seconds. The alternative has now cost four DDRs, and in this case
three gate runs spent debugging code that was not in the binary — while the real
fix sat on disk, already correct.
