= DDR-884 — OPEN-10's real signature is `op=create iter=0`, not churn (item 46)

**Status:** Accepted — narrowing evidence, instrument landed, root cause open
**Date:** 2026-08-10
**Follows:** DDR-880 and its correction.
**Scope:** `kernel/main.c` (churn probe diagnostics only). No behaviour change.

## What the one real capture actually says

CI run `31329941053` (`00808b4`, shard 0) is the first capture of OPEN-10's true
signature. The probe already printed more than anyone had read:

```
[boot-stamp] C ext4-done t=146
[boot-stamp] B proofs-begin t=167
[sfs] churn FAIL op=create iter=0
[sfs] btree churn FAIL
```

**`iter=0`.** The very first `vfs_create("/CHURN.TMP")` failed.

Iteration 0 creates one file into a fresh directory. `SFS_LEAF_MAX` is 14, so the
inode-entry B+tree cannot have split yet — nothing about a B+tree, a leaf split,
or churn is reachable at that point.

**OPEN-10 is not a churn defect and never was.** It is a first-create failure
that happens to be *reported by* the churn probe, because the churn probe is the
thing that creates a file at that moment. The name has misdirected this
investigation twice: once into "add a spinlock to `sfs.c`" (no target — `sfs.c`
has no global mutable state), and once into DDR-880's conflation with the lost
thread.

This also explains why 30 local runs of `smoke-sfs-btree-smp4` produced zero
occurrences while a `smoke-smpuser` boot produced one: the failure depends on
what else is touching the SFS root at that moment, not on churn depth.

## What was missing, and is now there

The probe recorded *which operation* failed but **discarded its return code**, so
three very different diagnoses were indistinguishable:

- `-EEXIST` — a leftover `/CHURN.TMP`, or a concurrent writer got there first.
- `-ENOSPC` — the volume.
- an **ADR-032 write-budget refusal** — neither of the above, and there is
  precedent: a write-budget exhaustion was once already mis-attributed to a
  "B+tree split bug" in this same probe.

`rc` is now printed. That is the whole change: one number that partitions the
hypothesis space, landed **before** re-running, so the next occurrence is
diagnosable instead of merely counted.

## Why the fix is not attempted here

Three candidate causes remain and the evidence does not separate them. Guessing
one and "fixing" it would produce a change whose only validation is that a ~1-in-
many-runs failure did not recur — which is indistinguishable from not having
fixed anything. That is precisely the trap DDR-866/867 cost this project a
working revert to learn.

**Next step is a capture, not a patch:** run `smoke-smpuser` and the other
full-boot `-smp 4` gates until `churn FAIL op=create` recurs, and read `rc`.

Regression green: `smoke`, `smoke-sfs-btree`, `smoke-sfs-btree-smp4`,
`smoke-numa`, `smoke-numa-alloc`, `smoke-fs-sfs-rw`, `smoke-user`,
`smoke-smpuser`. Zero warnings.
