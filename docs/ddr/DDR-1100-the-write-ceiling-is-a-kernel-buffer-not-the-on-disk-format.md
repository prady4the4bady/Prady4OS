# DDR-1100 — The ring-3 write ceiling is a KERNEL BUFFER, not the on-disk format

**Status:** assessment. No kernel change, no new gate, no defect in the write path.
One **one-line Makefile edit** — an `echo` string, §8 — so this is not purely
docs-only; `kernel.bin` is byte-identical before and after (`8283919d806459eb`),
verified by hash, because an `echo` in a recipe is not a build input.
**Corrects:** DDR-1098 §4, written one commit earlier, by this session.
**Scope:** Group B `smoke-sfs-largefile`; Group F audit-flusher row.

---

## §0 — What this is

DDR-1098 §4 measured the ring-3 write ceiling correctly and **attributed it to the
wrong thing**:

> "Lifting it is an ON-DISK FORMAT change (mkfs.sfs authors host images
> smoke-sfs-persist mounts without reformatting) and it is the Group B
> smoke-sfs-largefile row."

The number (16,384 B) is right. The attribution is **half wrong**, and it is wrong
in the expensive direction: it sends the next session to change an on-disk format
— breaking `smoke-sfs-persist`'s deliberately-unreformatted mount and requiring
`mkfs_sfs.c` and `sfs_readback.c` changes — when **two of the three ceilings it
fused together have no on-disk consequence at all.**

This is the DDR-1084 §1 pattern again (a stated blocker that is not true suppresses
work that is already unblocked). The difference, and the reason it is worth its own
record rather than a quiet edit: **DDR-1084 §1 described sessions inheriting a
stale blocker from an earlier DDR. Here the session that wrote the blocker and the
session correcting it are the same one, one commit apart.** The pattern is not
about elapsed time or lost context; it is about writing a remedy into a row while
working on a different subsystem, which is exactly what DDR-1098 was doing.

---

## §1 — THREE ceilings were fused into one sentence

Measured in the tree, not reasoned from any DDR:

| # | Ceiling | Value | Set by | On-disk consequence |
|---|---|---|---|---|
| 1 | ring-3 **single `write(2)`** | 4 × 4096 = **16,384 B** | `fd_write_user`'s `CHUNK` — a **one-PMM-page staging buffer** (`sys_io.c:116`) | **NONE** |
| 2 | **mkfs-authored** file | 4 blocks = **16,384 B** | `mkfs_sfs.c:122-128` emitting **one extent per block** (`block_count = 1`) | **NONE** |
| 3 | **appends per file** | **4**, for the life of the file | `inline_extents[4]` (`sfs.h:138`) | **YES — genuinely the format** |

DDR-1098 §4 named only the 16,384 B figure and attributed all of it to row 3.
**Only row 3 is a format constant.**

---

## §2 — The format already writes a 128 KiB extent, on every SFS gate boot

This is the load-bearing measurement, and it is not an argument — it is a call site
that runs in CI:

`write_extent` (`sfs.c:918`) takes an **arbitrary `uint32_t len`**, computes
`nblocks = ceil(store_len / SFS_BLOCK_SIZE)` (`:951`) and takes **one contiguous
run** via `alloc_run(c, nblocks)` (`:952`). `struct sfs_extent_ref` carries
`block_count` and `logical_len` as `uint32_t` (`sfs.h:121-127`). Nothing in the
extent path knows about 4096.

And it is exercised: `sfs_selftest_lz4` (`sfs.c:1555`) does

```c
sfs_write(c, &f, 0, s, 131072) == 131072      /* ONE call, ONE extent */
```

then asserts **three** things — `in->inline_extents[0].block_count < 32` (it is a
multi-block run), `sfs_read(...) == 131072 && memcmp(s, r, 131072) == 0` (byte-exact
readback), and a tag surviving a remount. `kernel/main.c:3002` prints
`[sfs] lz4+tags compress/readback/tag OK` from its result.

**So a 128 KiB single extent is written, read back byte-exact and asserted on every
SFS boot in this project.** A 4-extent file of that shape is 512 KiB. The format is
not what holds a ring-3 writer to 16 KiB.

---

## §3 — What actually holds ring 3 to 16 KiB, and its own history says so

`fd_write_user`'s `FD_VFS` branch allocates **one page** and loops:

```c
uint64_t kp = pmm_alloc_page();
const uint32_t CHUNK = 4096;
```

One `vfs_write` per chunk, and `sfs_write` makes **one extent per call**. So four
chunks exhaust the four extents, and the fifth returns `-EFBIG` — which is exactly
what DDR-1089's arm B pins.

**DDR-764 already moved this ceiling once, by this exact lever, with no format
change**, and its comment in that same function states the arithmetic:

> "the chunk is one 4 KiB block (from a PMM page, not the 16 KiB kernel stack) —
> 16x fewer `vfs_write` iterations, and each SFS extent now holds a full block so a
> ring-3 SFS file reaches **4 extents \* 4 KiB = 16 KiB instead of ~1 KiB**."

That is the strongest possible evidence for this correction, because it is the same
row's own history: the ceiling went ~1 KiB → 16 KiB by changing a kernel staging
buffer, and nobody touched the on-disk format to do it.

---

## §4 — mkfs has the same conflation, independently

`mkfs_sfs.c:122-128` authors a file as **one extent per 4 KiB block**:

```c
in->extent_count = (uint16_t)nblocks;
for (...) {
    in->inline_extents[e].block_start = data_start + e;   /* CONTIGUOUS */
    in->inline_extents[e].block_count = 1;                /* ...but 1 block each */
    in->inline_extents[e].logical_len = part;
}
```

The blocks it hands out are already contiguous (`data_start + e`), so the same
on-disk structure would accept **one extent of `block_count = nblocks`**. It emits
`nblocks` extents of 1 block instead, and `sfs_readback.c:90` then clamps
`e < in.extent_count && e < 4`. **A host-authored file is therefore capped at four
4 KiB blocks for the same avoidable reason as the ring-3 path** — the packing, not
the structure.

**Recorded, NOT fixed, and deliberately not claimed to work untested.** Changing
mkfs's packing needs its own verification against `smoke-sfs-persist` (which mounts
a host image *without reformatting*, so it is exactly the gate that would catch a
packing the kernel reader disagrees with). What is established here is narrower and
is all that is needed: **it is not a format change.**

---

## §5 — What this does and does not do for the flusher

DDR-1098 §4 said a ~240 KiB flushed audit log is "FIFTEEN TIMES the ceiling". That
stands as arithmetic, and the correction splits its consequences:

- A flusher writing the log in **one large `write(2)`** is blocked only by ceiling
  #1 — a kernel buffer, **free to lift, no on-disk change.**
- A flusher **appending incrementally** is blocked by ceiling #3 — genuinely the
  format, and DDR-1098 §4's remedy is the right one **for that shape only.**

So the Group F row is not unblocked, and this does not claim it is. What changes is
that the row now has two shapes with two different costs instead of one shape with
the expensive cost attached to it.

---

## §6 — NOT BUILT: `CHUNK` is not raised, and the reason is a cost, not a difficulty

Raising `CHUNK` to 64 KiB would take ceiling #1 to 256 KiB in one line
(`pmm_alloc_pages(4)` instead of `pmm_alloc_page()`). It is **not done**, on
DDR-1069's test and on a measured cost:

1. **Nothing shipping needs it.** No ring-3 program in this tree issues a single
   `write(2)` larger than 4096 to an `FD_VFS` fd; `bigwritetest` writes in
   4096-byte calls by design. The flusher is not built. A capability exercised only
   by its own gate is what DDR-1069 declined to ship.
2. **It buys a failure mode on a path that currently has none.** It would demand
   **16 physically contiguous pages on every `FD_VFS` write syscall**. `pmm_alloc_page()`
   (order 0) effectively cannot fail; an order-4 run can, under fragmentation — and
   `fd_write_user` returns `-ENOMEM` when it does. That converts a working write
   into a failure, on the write path, for a ceiling nothing is currently hitting.
   A try-order-4-then-fall-back-to-order-0 form removes the failure and makes the
   ceiling **best-effort**, i.e. a file's maximum size would depend on memory
   fragmentation at the moment of writing — which is worse to reason about than a
   fixed 16 KiB, and would make any gate arm asserting a size intermittent.
3. **ADR-032 interacts and the numbers are close enough to matter.** The token
   bucket is `FS_WRITE_BURST_MAX = 1 MiB` refilling at `256 KiB/tick`
   (`sched.h:22-23`). A 256 KiB single write consumes a quarter of a full bucket in
   one call. Not a blocker; not nothing either, and it is the kind of interaction
   that should be measured before the constant moves rather than after.

If it is ever built, the gate arm is already obvious and is **not** "write more than
16 KiB and assert success" — that arm would pass on a build with any `CHUNK` ≥ the
size written. What only a raised `CHUNK` can produce is **a file larger than 16 KiB
whose `extent_count` is less than 4**, i.e. the inode showing fewer extents than
4 KiB chunking could possibly have produced.

---

## §7 — NOT CLAIMED

- **No kernel change.** `kernel.bin` is byte-identical (`8283919d806459eb`, hash-
  verified before and after `smoke-shell`), so the size/headroom pair and
  `ci-docstate-check` are unaffected. GLOBAL_FORBIDDEN 76; **179 gates unchanged**;
  no new gate, and **no assertion is added or removed** — §8 changes an `echo`.
- **No defect is found and none is alleged.** `fd_write_user`, `write_extent`,
  `sfs_write`, `mkfs_sfs.c` and DDR-764 are all correct. 16,384 B is a documented
  ceiling, not a bug, and DDR-1089 already reports it with an exact errno.
- **DDR-1098 is not withdrawn.** Its cursor, its gate arms, its §4 *measurement* and
  its "four extents for the life of the file" all stand. What is corrected is one
  clause of one sentence: the **remedy** it attributed to that measurement.
- **The Group B `smoke-sfs-largefile` row is CORRECTED, not closed**, and no gate is
  built for it. The Group F flusher row is **not unblocked** (§5).
- **`CHUNK` is not raised** and `inline_extents[4]` is not widened. mkfs's packing is
  **recorded, not changed**, and is explicitly not claimed to work untested (§4).
- **No gate was run for this DDR.** What was measured is: `write_extent`'s body and
  its callers; `sfs_write`'s extent-count guard; `sfs_selftest_lz4` read in full and
  its `main.c` sentinel; `fd_write_user`'s `FD_VFS` branch read in full; `sfs.h`'s
  inode and extent layout; `mkfs_sfs.c`'s inode authoring and `sfs_readback.c`'s
  clamp; `sched.h`'s ADR-032 constants; and `bigwritetest.c`'s arm B. The 128 KiB
  extent's green status comes from CI having run the SFS gates, not from a run here.
- **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.

---

## §8 — One `echo`: the gate was understating its own coverage

Found by running `smoke-shell` for this commit rather than by reading anything.
DDR-1098 added two `PRADYOS_AUDIT` arms at `Makefile:1695` and `:1699`, and did
**not** add them to that gate's PASS line at `:1903`, which enumerates what the gate
covers:

> `[shell] PASS — … + wait(DDR-1068) + source(DDR-1087), clean, no panic.`

The arms run and pass — `rc=0` proves that, and they are what caught mutant M1.
What was wrong is the gate's **statement about itself**: a reader auditing
`smoke-shell`'s coverage from its own output would not learn that the audit cursor
is asserted there. That is this DDR's subject one level down — **a record drifting
from what was built, written by the session that built it** — so it is fixed here
rather than left for the audit that would eventually find it.

Fixed to `… + source(DDR-1087) + audit(DDR-1098), clean, no panic.`

**Safe, measured not assumed:** `grep -rn 'shell\] PASS'` over `tools/` and
`.github/` returns **nothing** — no script, workflow or checker parses that string,
so it is a human-facing label with no consumer to break. `smoke-shell` re-run after
the edit: `rc=0`, scan clean at 76 patterns, and `kernel.bin` **identical before and
after**.

**NOT CLAIMED:** no arm is added, removed or changed; the gate's behaviour is
exactly what DDR-1098 shipped, and only its self-description changes.
