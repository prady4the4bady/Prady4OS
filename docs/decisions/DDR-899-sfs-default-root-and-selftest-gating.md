= DDR-899 — provisioned SFS as default boot root; gate the destructive self-tests

**Status:** ACCEPTED (design). **Implementation deferred within this slice** —
a CI run was in flight, and this change requires re-running 12 gates to verify,
which needs QEMU.
**Date:** 2026-08-16
**Scope:** CLAUDE.md §6.2 item 1 (D1).
**Lineage:** DDR-770/771 (provisioned root mechanism) + DDR-804 (fw_cfg probe
selection) → **DDR-899 (this)**.

## The defect

`kernel/main.c:1128` formats the SFS disk **unconditionally on every boot**:

```c
if (blk_count() > 2) {
    struct blk_device *sbd = blk_get(2);
    if (sbd && sfs_format(sbd) == 0) {          /* <-- destructive, always */
```

`main.c:1002` already admits the consequence in a comment: *"the SFS mount is
later reformatted by the destructive self-tests"*. So a provisioned SFS root
cannot survive a boot, and `QEMU_SFSROOT` remains a special path rather than the
normal one. That is what blocks §6.2-1.

## Mechanism: reuse `probe_enabled`, do not invent a new transport

The work queue proposed a `QEMU_SFS_SELFTEST=1` knob "consistent with
QEMU_QMP_DIAG / QEMU_SMP". Those are **boot_test.sh-level** knobs that shape the
QEMU command line; they cannot be read from inside the kernel.

The in-tree mechanism for an in-kernel conditional already exists:
`probe_enabled(name)` (`kernel/drivers/fwcfg/fwcfg.c:113`), fed over **fw_cfg**
from the `QEMU_PROBES` list (DDR-804, plumbed at `boot_test.sh:297-306`). The
destructive block becomes:

```c
if (probe_enabled("sfsselftest") && blk_count() > 2) { … sfs_format(sbd) … }
```

and any gate that needs it sets `QEMU_PROBES=sfsselftest`. This adds no new
transport, no new parsing, and follows the pattern every other opt-in probe uses.

## Blast radius — 12 gates, enumerated

This is the part that makes D1 a real change rather than a two-line edit. Gates
that assert on `[sfs]` sentinels produced by the boot-time self-test block, and
which must therefore opt in (or be re-pointed at a non-destructive source):

```
smoke-blkmq            smoke-sfs-btree
smoke-blkmq-trace      smoke-sfs-btree-smp4
smoke-fs               smoke-sfs-dirs
smoke-fs-sfs-rw        smoke-sfs-gc
smoke-msixap           smoke-sfs-unlink
smoke-shell            smoke-user
```

Each needs `QEMU_PROBES=sfsselftest` added **and** an individual run to confirm
it still passes — a gate that silently stops exercising SFS while still passing
is worse than the current state. `smoke-shell` and `smoke-user` in particular
assert on `[sfs]` output incidentally, so they must be checked rather than
assumed.

Note `ci-shard-check` must still pass afterwards: no gate may be added or
removed, only its environment changed.

## The new gate

`smoke-sfs-boot-root`: boot **without** `sfsselftest` in `QEMU_PROBES`, with a
provisioned SFS image, and assert:

- `PRADYOS_SFS_ROOT_OK` — the root mounted from the provisioned image.
- **Forbidden:** any `[sfs] mounted … on blk2` / format-path sentinel, proving
  the destructive block genuinely did not run. A gate that only asserts the
  positive would pass even if the self-test still reformatted underneath it,
  which is precisely the failure mode this DDR exists to remove.

## Ordering

D1 must land before §6.7-1 (AETHER audit-ring persistence to SFS) and before
§6.7-2 (agent `execve`-on-respawn from SFS), both of which are recorded as
depending on a stable SFS root.

## Verification bar

Build warning-clean; `ci-shard-check` PASS; the new gate 3x; **all 12 listed
gates re-run individually**; plus the standing freeze-site trio.
