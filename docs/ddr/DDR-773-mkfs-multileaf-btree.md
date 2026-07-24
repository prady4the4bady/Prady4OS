# DDR-773 — mkfs.sfs multi-leaf B+tree (>14 provisioned slots)

**Status:** implemented — `smoke-mkfs-sfs` PASS (20 files / 41 slots → 3 leaves +
internal root at block 46; first/middle/last read back byte-exact) and
`smoke-sfs-persist` PASS with a genuinely multi-leaf image (`21 slots, root=23
(multi-leaf)`) — i.e. the **kernel** mounted and descended a host-authored
two-level B+tree. Both tools build `-Wall -Wextra` clean. The ≤14-slot layout is
byte-identical to before (`root=1`), so existing images are unaffected.
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 2**
("mkfs.sfs multi-leaf B+tree (>14 slots)" — High priority, requires host tool to
mirror the kernel `sfs.c` split path exactly).
Follows DDR-767 (mkfs.sfs) / DDR-769 (nested dirs) / DDR-770 (provisioned root).

## Problem

`mkfs.sfs` emits a **single root leaf**. `SFS_LEAF_MAX = 14` slots is a hard
ceiling, and the tool `exit(1)`s past it ("root leaf full"). Every provisioned
file costs 2 slots (INODE + DIR) and every directory another 2, so the practical
cap is ~6 files — far too few to provision a real `/etc` tree. The kernel already
*reads* multi-level trees (`bt_search_root` descends internal nodes); only the
host writer is limited.

## Kernel format to mirror (read from `kernel/fs/sfs/sfs.c`, must match exactly)

- **Internal node:** `flags = SFS_NODE_INTERNAL`, `nkeys` = separator count,
  `child0` = leftmost child block, `u.intern[k] = {sep, child}` where `child`
  holds keys **>= sep**. `SFS_INT_MAX = 254`.
- **Descend** (`bt_search_root`): `i=0; while (i < nkeys && key >= intern[i].sep) i++;`
  `blk = (i == 0) ? child0 : intern[i-1].child`.
- **Leaf:** `flags = SFS_NODE_LEAF`, `nkeys` slots sorted by key, `next_leaf`
  chains to the following leaf. Lookup inside a leaf is a linear scan, so slot
  order within a leaf does not affect reads — but keys must be globally sorted
  across leaves for the descend to land correctly.
- Kernel splits at `half = cnt/2` with separator `w[half].key`. mkfs does a
  **bulk load**, not an incremental split: it produces a valid tree of the same
  shape family, which the kernel then splits normally on later inserts.

## Decision — `tools/mkfs_sfs/mkfs_sfs.c`

1. Replace the single `struct sfs_node g_root_leaf` with a flat, bounded slot
   array `g_slots[MKFS_MAX_SLOTS]` (`MKFS_MAX_SLOTS = 512`; `leaf_add` and
   `find_dir` operate on it). 512 slots ≈ 250 provisioned files — well past any
   build-time need, and a hard bound that errors cleanly (**S2**).
2. Finalize (`write_tree`): sort all slots by key, then
   - `cnt <= SFS_LEAF_MAX` → **byte-identical to today**: one leaf at block 1,
     `sb->root_btree = 1` (no regression for existing images);
   - else → chunk into `ceil(cnt / SFS_LEAF_MAX)` leaves (leaf 0 at block 1, the
     rest from `next_free`), chain `next_leaf`, then write **one internal root**
     with `child0 = leaf[0]` and `intern[k] = {sep = first key of leaf[k+1],
     child = leaf[k+1]}`; `sb->root_btree` = that block.
   - A single internal root spans `SFS_INT_MAX + 1 = 255` leaves = 3570 slots, so
     one level is always enough at `MKFS_MAX_SLOTS = 512`; error out (never
     silently truncate) if that were ever exceeded.
3. `tools/mkfs_sfs/sfs_readback.c` gains the same descend loop so the host
   verifier follows internal nodes (it currently assumes the root is a leaf).

## Gates

- **`smoke-mkfs-sfs`** (host, ~1 s, deterministic): provision **20 files** (=41
  slots with the root-inode slot → forces 3 leaves + an internal root) and read
  back the **first, a middle, and the last** file byte-for-byte via
  `sfs_readback`, proving the internal descend lands correctly at both edges.
- **`smoke-sfs-persist`** (existing kernel boot gate): provision enough extra
  files that the image the **kernel** mounts is genuinely multi-leaf, so
  `PRADYOS_SFS_PERSIST_OK` + `PRADYOS_SFS_NESTED_OK` now also prove the kernel
  reads a host-authored *multi-level* tree.

## Blast radius / risk

Host-tool only — **zero kernel files touched** (`tools/mkfs_sfs/*.c` + Makefile).
Worst case is an unreadable image, which both gates catch (the host verifier
mirrors the kernel read path; the boot gate uses the real kernel). Fallback: the
`cnt <= 14` path is unchanged, so reverting is a one-file revert.

## Architecture prerequisite checklist

- New NSI/syscalls: **none** (NSI range stays at 75).
- TCB / roster / agent-slot fields: **none**.
- PMM/VMM shared mappings: **none** (host tool).
- Capability gates: **none** — build-time tool, no runtime authority.
- AETHER queue/audit record types: **none**.
- Scheduler/accounting hooks: **none**.
- Filesystem/root-mount constraints: **yes** — depends on the SFS on-disk format
  (ADR-018), hierarchical dirs (DDR-738), and the provisioning chain
  (DDR-767/769/770). Format parity with `sfs.c` is the whole risk surface.
- Network policy tables: **none**.
- Compositor/UI exposure: **none**.
- New smoke gate: reuses/extends two existing deterministic gates (no new
  TCG-timing dependence).
- **Security invariants:** **S2 (bounded everything)** applies and is honoured —
  `MKFS_MAX_SLOTS` is a hard bound that errors cleanly instead of overflowing.
  S1/S3–S8 are not engaged: no runtime authority, no capability, no agent path,
  no audit surface. Nothing here weakens W^X, NX, or any capability contract.

## Non-goals

- Multi-**level** internal trees (>255 leaves) — one internal root is sufficient
  at this bound; deeper trees are the kernel's job on later inserts.
- Mirroring the kernel's incremental CoW split step-for-step (bulk load instead).
- Changing any kernel-side B+tree code.
