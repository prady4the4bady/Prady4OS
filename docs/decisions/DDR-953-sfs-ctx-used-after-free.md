# DDR-953 — `smoke-agentmetrics` red is an SFS use-after-free, not a missing sentinel

Status: ROOT CAUSE CONFIRMED (mechanism). Fix NOT yet written — see §f.
Evidence: CI run **32086772141**, shard 2, gate `smoke-agentmetrics`, tip 889a059.

## a. The verdict line is misleading

The gate reports:

```
[smoke] FAIL — required pattern 'AGENT_METRIC KRYOS sched ok' not found.
```

which reads as an agent-metrics defect. It is not. The serial log ends in

```
*** NEXUS KERNEL PANIC ***  #PF  error=0x0  RIP=0xFFFFFFFF8002CD18
CR2=0x0000000053465341  RAX=RDI=0x0000000053465331
```

The kernel **panicked** before the sentinel could print. `AGMETRIC.ELF` had
already loaded cleanly at t=2605. Any gate running after the panic point would
have reported its own sentinel missing; `smoke-agentmetrics` is the messenger.

## b. Same tip, both verdicts — this is an intermittent

Runs 32086799351 (**success**) and 32086772141 (**failure**) are both
`workflow_dispatch` on **889a059**. Identical tree, opposite verdicts. So this
is not a regression from any change, and it is not caused by the unpushed
DDR-951/952 work.

## c. It is NOT item 47

Applying the established discriminator to this capture:

```
[boot-stamp] A probe-block-begin t=2247
[boot-stamp] B proofs-begin      t=2721      A→B gap = 474 ticks
[hb] 500 1000 1500 2000 2500 …               continuous, no gap
```

B is present and the gap is **474**, well inside the ≲1450 "baseline, not a
stall" branch. `[hb]` never stops. Item 47 is not implicated and must not be
credited with this failure.

## d. The mechanism, from the register file alone

`RIP` resolves to **`rd_block_bd`** (`kernel/fs/sfs/sfs.c:83`), +0x18 into a
0x40-byte function:

```c
static void rd_block_bd(struct blk_device *bd, uint64_t blk, void *buf) {
    bd->read(bd, blk * SFS_SECTORS_PER_BLOCK, buf, SFS_SECTORS_PER_BLOCK);
}
```

Three independent facts agree:

1. `struct blk_device` (`kernel/drivers/blk/blk.h:10`) is
   `name`@0x00, `capacity_sectors`@0x08, **`read`@0x10**.
   The fault address is `CR2 = RDI + 0x10` exactly — the `bd->read` load.
2. `RDI = RAX = 0x53465331` = **`SFS_MAGIC`** (`sfs.h:15`, `"SFS1"`).
   So `bd` is not a pointer; it holds the SFS superblock magic.
3. `struct sfs_ctx` (`sfs.c:56`) begins with `struct blk_device *bd;` at
   **offset 0**. Therefore `c->bd` is literally `*(uint64_t *)c`.

Composing them: a `struct sfs_ctx *` was dereferenced while pointing at memory
whose first 8 bytes are `SFS_MAGIC` — i.e. **at a buffer holding a superblock**.
`err=0x0` (not-present, not a protection violation) is consistent: the magic is
not a mapped address.

`RSI = 0x16A = 362`, a data block, not block 0. So the call came through
`rd_block(c, …)` on a stale context, not through one of the superblock reads.

## e. Why a stale context is reachable

`sfs.c:324`:

```c
static void sfs_umount(void *ctx) { kfree(ctx); }
```

The context is freed with **no invalidation of any other reference** — nothing
NULLs the mount's private pointer, and there is no refcount. Every other entry
point re-derives `c` by casting the stored `ctx` (11 call sites,
`sfs.c:293,304,314,666,691,772,800,916,974,1052,1312`). A freed chunk of
`sizeof(struct sfs_ctx)` later handed back by `kmalloc` and filled with a
superblock read reproduces the observed register state precisely.

## f. What is NOT yet established — do not fix on this alone

Confirmed: the faulting pointer is an `sfs_ctx *` aliasing superblock bytes, and
`sfs_umount` frees without invalidating.

**Not confirmed:** which allocation reused the chunk, and which caller reached a
freed context. Writing the fix now would violate §6.0-B (instrument-first) and
CLAUDE.md §0.2. Required next, in order:

1. Poison on free: `sfs_umount` writes a distinct pattern over the ctx before
   `kfree`, so a stale deref faults on the poison rather than on whatever the
   allocator happened to hand out. This converts an intermittent into a
   deterministic, self-identifying panic and costs nothing when correct.
2. Capture a failing run with the poison active to name the caller.
3. Only then choose between NULLing the mount's private pointer, refcounting the
   context, or ordering unmount behind quiescence.

The fix is deliberately left unwritten until step 2 produces a caller.

## g. Relationship to existing OPEN items

This is a strong candidate for the unattributed SFS-churn / `FSRM FAIL`
intermittents already on record. It is NOT claimed as their cause here — one
capture cannot establish that (RULE 20), and those entries stay open pending a
capture carrying the poison from §f step 1.
