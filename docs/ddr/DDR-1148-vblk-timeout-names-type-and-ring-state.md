# DDR-1148 — The virtio-blk timeout line names the request type and the ring state

Status: DESIGN (committed before code, NON-NEGOTIABLE 5). Instrument only.
**No fix. No mechanism named.**

## 1. Artefact

CI 36238336846 (`pull_request` suite on `dfbe417`), shard 9, `smoke-kill`,
gate 10 of 18, `kernel.bin: OK`. The boot was single-CPU (`rqcpus=1`).

After `[sfs] mounted sfs on blk2`, the SFS create/lookup self-test never
finished. The capture then shows this line, repeating:

```
[vblk] compl wait timeout unit=2 dest_cpu=0 dest_dticks=500 ... lba=0
```

It appears at t≈1509, 2485, 3489, 4481 and 5481, and the BSP kept ticking the
whole time.

**The sibling `push` suite on the same SHA (36238334044) was green on shard 9.**
This is one binary with two outcomes, the DDR-1009 class. Locally, smoke-kill
passes 3/3 on that binary. The count is one occurrence and no rate is claimed.

## 2. Why the existing line cannot say what happened

`dest_dticks=500` equals the full wait window, and CPU 0 is the only CPU. So the
CPU was alive and taking its own timer interrupt. That rules out DDR-976's
halted-AP reading. The remaining readings are:

- **(a) The device never completed the request.** The host stalled, for example
  because QEMU `cache=writeback` turned a FLUSH into an `fsync` on a loaded CI
  runner.
- **(b) The device completed but the completion was never reaped.** The used
  ring advanced and the MSI-X was lost or not handled.

The line cannot separate these two readings, for two reasons:

1. `lba=0` is shared. A FLUSH carries `sector = 0` (`submit()` stores `lba`,
   and `vblk_flush` passes 0), and an SFS superblock write is also lba 0. DDR-1143
   piece 2 added FLUSH on the SFS commit path, so the line cannot tell which one
   timed out.
2. Nothing on the line reads the ring. Reading (b) leaves `used->idx !=
   last_used`, reading (a) leaves them equal, and the line prints neither value.

The repetition is also unexplained. Each timeout returns `-EIO` and releases the
slot, so the ~1000-tick spacing means *successive* requests each time out. Under
reading (b), every later completion would also have been missed. Under reading
(a), the device would be stuck. Either way the line does not say.

## 3. The instrument

The existing line is built from ~20 `kputs` calls and is left **unchanged**
(four DDRs quote it; changing it would be DDR-1123 §2's objection). A **second**
line is emitted with one `kline_emit`, immediately after it, on the same failure
path:

```
[vblkto] unit= type= status= head= used_idx= last_used= avail_idx= late= tmo=
```

| field | meaning |
|---|---|
| `type` | `VIRTIO_BLK_T_*` of the timed-out request: 0 IN, 1 OUT, 4 FLUSH |
| `status` | the status byte the device writes. `0xFF` is the driver's pre-fill, so `0xFF` means the device never wrote it |
| `head` | the descriptor head of the timed-out request |
| `used_idx` / `last_used` | the device's `used->idx` and the driver's reap cursor. **Unequal means reading (b).** |
| `avail_idx` | how far the driver has published |
| `late` | per-unit count of completions reaped for a slot that no longer holds that head (§4) |
| `tmo` | per-unit timeout count |

**Nothing is judged.** The line reports values; the reader compares them.

The prefix `[vblkto]` matches no `GLOBAL_FORBIDDEN` pattern (checked: `grep -i
vblk` over the 77 entries returns nothing). So the line changes **no verdict**,
exactly as the existing timeout line does not.

**Cost.** Nothing on the healthy path prints. The healthy path pays one extra
store per submit (`req[s].head`, `req[s].type`) and one compare per reaped
completion. There is no new lock, and `compl_lock` is already held at both sites.

## 4. A latent hazard found while designing this. NOT fixed.

On timeout, `submit()` releases slot `s` but leaves `head2slot[head] = s`. If
the device completes that request later, `complete()` marks `req[s].done = 1`,
possibly after slot `s` has been **reused by a different request**. That would
complete the new request with the old request's status and without its data
transfer, so a read could return a buffer the device never filled.

This is read in the source. **It has not been observed.** No capture shows
a late completion, so NON-NEGOTIABLE 3 forbids fixing it here. The `late=`
field is exactly the measurement that would show whether it ever happens: it
counts reaped heads that do not match the slot's current head, or that arrive
for an unused slot. Behaviour is unchanged; the stale completion is still
processed as before. If `late > 0` ever prints, the capture is the artefact, and
the fix (mark the head abandoned on timeout, reap it without touching the slot)
gets its own DDR.

## 5. Proof

This is a forced mutant on a recorded hash, because the trigger cannot be
manufactured in product (DDR-1105 §8's reason):

- **M1** makes one request on unit 2 wait forever. The completion is suppressed
  by not reaping, so `used_idx` should advance past `last_used` and the line
  should read reading (b).
- **Negative.** The shipping kernel prints **zero** `[vblkto]` lines across the
  regression set, and the kernel hash is pinned (DDR-1060 §9).

There is no gate arm. The line appears only on a failure path that no healthy
boot takes, and asserting the absence of a rare intermittent is unfalsifiable at
any affordable N (DDR-1082).

## 6. NOT CLAIMED

- No fix and no mechanism named. The shard-9 red is not attributed to DDR-1143
  pieces 2 or 3, and it is not exonerated either (DDR-1042). Piece 2 added a
  FLUSH on exactly this path, so it remains a candidate.
- No rate: one occurrence.
- §4 is not fixed.
- The existing `[vblk] compl wait timeout` line is unchanged.
- `GLOBAL_FORBIDDEN` stays at 77. The gate count stays at 183.
