# DDR-962 — gate `sfs_rename`

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).
Closes the item DDR-956 opened and DDR-958 could not.

## 1. Problem

`sfs_rename` has been implemented, callable and **ungated** since DDR-956.
DDR-956 designed a gate for it and withdrew it, correctly: PRISM runs on the FAT
default mount, so a shell-driven gate could only prove that `mv` fails, and the
one arm that would have fired (`ENOENT`) fires whether or not rename works —
vacuous by R8.

DDR-958 then shipped `fat32_rename` with `smoke-rename`, which drives PRISM's
`mv` and therefore proves **FAT only**. It said so explicitly rather than
implying coverage. The SFS half needed an embedded ring-3 probe, and no probe
fit: `kernel.bin` was 3,714 B under the stage-2 read window and a probe costs a
page-aligned 8,192 B.

DDR-960 raised the window to 1.5 MiB. This is the probe that raise was for.

## 2. Design — one probe, SFS-rooted, opt-in

`user/renametest.c`, freestanding, no writable globals (DDR-826), spawned from
`main.c` behind `probe_enabled("rename-sfs")` with `root_mnt = smnt` set before
`sched_unblock` — the same shape as the DDR-866 ftruncate probe two blocks
above it, for the same two reasons: SFS is not the default root, and a probe
that creates files on the shared SFS root must not run in every other gate's
boot.

### The three arms, and what each one catches

| sentinel | arm | catches |
|---|---|---|
| `PRADYOS_SFS_RENAME_OK` | the destination reads back the source's **exact bytes**, and the source stops opening | a rename that creates an entry but drops the inode; a copy-style rename that leaves the source |
| `PRADYOS_SFS_RENAME_ENOENT` | renaming an absent path **fails** | the stub — a handler returning 0 unconditionally |
| `PRADYOS_SFS_RENAME_LFN` | a long source name is retired: the old name stops resolving, the new one holds the payload | the leaf-slot name copy, `name_len`, and FNV1a32 keying of a name outside the 8.3 shape |

**Arm 1 reads the payload rather than just opening the destination.** That is the
difference between proving a name exists and proving the *same inode* was
re-keyed, which is what `sfs_rename` claims to do (two `bt_insert`s and one
`sfs_commit`, inode reused not copied). An open-only check passes for an
implementation that inserts an empty entry and loses the data.

**Arm 3 mirrors `smoke-rename` arm 6 in intent, not in mechanism.** On FAT the
hazard is concrete: stale VFAT fragments make a renamed file answer to its old
name. SFS has no fragment chain — it stores the whole name in the B+tree leaf
slot with an explicit length — so there is no equivalent corruption to
reproduce. What a long name exercises here is the `name_len` field and the
byte-by-byte name copy in `bt_insert`, on a name a FAT-shaped implementation
would have truncated to 8.3. Unlike FAT, the probe can create its own long-named
file, so this arm needs no image fixture and cannot go vacuous through a missing
one.

`holds()` distinguishes "did not open" from "opened with the wrong bytes" and
**fails the probe** on the latter rather than returning false. Conflating a
corrupted rename with an absent file would let arm 1's negative check
("source still readable") pass for the wrong reason.

## 3. Gate

`smoke-rename-sfs`, **shard 5**, 90 s. Shard 5 rather than 4 because
`smoke-rename` already sits on 4 and both carry DDR-956 §5's forbidden
sentinels, which make them ineligible for the DDR-785 early exit — so each burns
its full window and putting both on one shard would make it the critical path.

Gate count: 146 → **147**, 6 shards, 6 excluded.

## 4. Verification

### R8 two-arm: the gate must be able to fail
`sfs_rename` was stubbed to `return 0` without doing anything, the kernel
rebuilt, and the gate re-run:

```text
[smoke] FAIL — required pattern 'PRADYOS_SFS_RENAME_OK' not found.
SFS RENAME FAIL: destination does not exist after rename
```

Caught at arm 1, with a diagnostic that names the actual defect rather than
just reporting a missing sentinel. Stub reverted, kernel rebuilt, gate re-run
green — a revert is not verified until the gate is re-run.

### Measured (kernel `78544f73b2b43c625260875530e0467c`, `kernel.bin` 1,053,054 B)

| check | result |
|---|---|
| `make image` | rc=0, 0 warnings at `-Werror` |
| `make smoke-rename-sfs` | **PASS** — 3/3 sentinels |
| `make smoke-rename-sfs` (stubbed backend) | **FAIL** at arm 1, as required |
| `tools/ci/fs_regression.sh` | **9/9 PASS**, uaf=0 |
| `make ci-shard-check` | OK — 147 gates / 6 shards / 6 excluded |

### The DDR-960 loop, closed
`kernel.bin` with this probe embedded is **1,053,054 B** — the exact figure
DDR-958 recorded as *"4,478 B over"* the old 1 MiB window. It now builds, boots
and gates, with **519,810 B spare — room for 63 more probe ELFs**. The raise was
not theoretical headroom; this is the first thing it bought.
