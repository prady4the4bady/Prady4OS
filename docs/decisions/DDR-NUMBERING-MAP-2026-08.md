= DDR numbering correction — the 2026-08 series moved from 885-916 to 917-933

**Date:** 2026-08-16

## What happened

Across several sessions I allocated DDR numbers 885-900 and 916 for the
g_ticks / flake-gate investigation **without checking that those numbers were
already in use**. They were. Each of the following had two files in
`docs/decisions/` with the same number and different subjects — e.g.
`DDR-887-iso-pipeline-blocked.md` (pre-existing) alongside
`DDR-887-gticks-freeze-is-the-common-root-cause.md` (mine).

That makes the decision record ambiguous: a future session reading "DDR-887"
gets the wrong document half the time. It is a real defect in the project's
memory, not a cosmetic one.

## The correction

My files were renamed into the free 917+ block. Cross-references *inside* the
renamed files were updated. Nothing pre-existing was touched.

| was (COLLIDED) | now | subject |
|---|---|---|
| DDR-885 | **DDR-917** | rqstress must observe completion |
| DDR-886 | **DDR-918** | probe verdicts must distinguish late from wrong |
| DDR-887 | **DDR-919** | g_ticks freeze is the common root cause |
| DDR-888 | **DDR-920** | vfs_create rc conflates precondition and driver |
| DDR-889 | **DDR-921** | g_ticks increment must be atomic |
| DDR-890 | **DDR-922** | spin instrument + stray-QEMU guard |
| DDR-891 | **DDR-923** | churn rc comes from SFS, not VFS |
| DDR-892 | **DDR-924** | bound the off-CPU spin |
| DDR-893 | **DDR-925** | call-rate refutes the suppression theory |
| DDR-894 | **DDR-926** | evresize must observe the corner |
| DDR-895 | **DDR-927** | cadence must use a clock, not a loop count |
| DDR-896 | **DDR-928** | widen aclick dump; record the rate margin |
| DDR-897 | **DDR-929** | drag must observe the title bar |
| DDR-898 | **DDR-930** | blkint workers never returned |
| DDR-899 | **DDR-931** | SFS default root + self-test gating |
| DDR-900 | **DDR-932** | spawn does not wake idle CPUs |
| DDR-916 | **DDR-933** | console GAP B unconditional RX drain |

## Commit history is NOT rewritten

Force-push is forbidden, so every commit message from this series still cites
the OLD number. **Use this table to resolve them.** A commit saying
"Refs: DDR-887" means DDR-919 (g_ticks freeze), not DDR-887 (ISO pipeline).

## Not a collision

`DDR-714` and `DDR-774` appear multiple times by design — they are staged
sub-DDRs with distinct suffixes (`DDR-714C1`, `DDR-714C2`, `DDR-714C3`). Those
were left alone.

## Rule going forward

Before allocating a DDR number, check BOTH `docs/decisions/` and `docs/ddr/`:

```sh
ls docs/decisions/ docs/ddr/ | grep -oE 'DDR-[0-9]{3}' | sort -u | tail -5
```

Next free at the time of writing: **DDR-934**.

## Source-code comments also cite the OLD numbers — deliberately left alone

In-tree comments in `kernel/`, `user/`, `tools/` and the `Makefile` still cite
the pre-renumber numbers for this series (e.g. `sched.c` says `DDR-887` where the
document is now DDR-919). **They were NOT rewritten, on purpose.**

A blanket substitution over the 885-900 range would corrupt correct references,
because source citations in that range are a MIX of this series and pre-existing
DDRs of the same number. Verified examples of references that must NOT change:

| file | cites | actually means (pre-existing) |
|---|---|---|
| `kernel/fs/sfs/sfs.c` | DDR-889 | SFS on-disk freelist |
| `kernel/acpi/acpi.c` | DDR-892 | ACPI S3 and P-states |
| `user/init.c` | DDR-891 | service manager |
| `user/prism.c` | DDR-888 | PRISM agent DSL |
| `user/compositor.c` | DDR-893 | manual-mode layout |

Distinguishing mine from those requires per-reference judgment, and getting it
wrong silently breaks a correct citation — a worse outcome than a stale one that
this table resolves.

**Rule for readers:** when a source comment cites 885-900 or 916, check the
subject. If it matches a row in the table above, use the pre-existing DDR. If it
matches this series' subject (g_ticks, spin, probe verdicts, cadence, drag,
evresize, churn rc, blkint), add the offset from the mapping table.

New code from DDR-934 onward cites the correct current numbers.
