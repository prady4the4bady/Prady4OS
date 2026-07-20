# DDR-766 — NVMe I/O queue + read/write + blk_register (M2 driver 2/2)

**Status:** implemented — `smoke-nvme` extended, PASS (`registered nvme0 (32768
sectors)` + `PRADYOS_NVME_RW_OK`); image builds `-Werror`-clean. 100 gates.
**Layer:** 3 (drivers). M2 storage completeness. Second of two NVMe slices;
builds directly on DDR-765 (controller bring-up + Identify).

## Problem

DDR-765 brings the NVMe controller up and identifies the namespace but does no
block I/O and does not register with the generic block layer. This slice makes
the NVMe namespace a real `struct blk_device` — a second block backend alongside
virtio-blk — so the VFS/SFS/FAT stack can sit on NVMe.

## Decision — extend `kernel/drivers/nvme/nvme.c`

After Identify succeeds:

1. **Create one I/O queue pair** via admin commands (qid 1, 64 entries, one
   identity-mapped PMM page each):
   - **Create I/O Completion Queue** (admin opcode 0x05): `PRP1 = cq_phys`,
     `CDW10 = (qsize-1)<<16 | qid`, `CDW11 = PC(bit0)=1` (physically contiguous;
     IEN=0 — we poll, no NVMe IRQ this slice).
   - **Create I/O Submission Queue** (admin opcode 0x01): `PRP1 = sq_phys`,
     `CDW10 = (qsize-1)<<16 | qid`, `CDW11 = PC=1 | CQID(qid)<<16`.
   The I/O queue has its own tail/head doorbells at `0x1000 + (2*qid + isCQ)*stride`.

2. **`nvme_io(write, lba, buf_phys, sectors)`** — issue NVM Read (0x02) /
   Write (0x01) on the I/O SQ, poll the I/O CQ phase bit. **PRP simplification
   (bounded, correct for any alignment):** each command covers only up to the
   next 4 KiB page boundary from `buf_phys`, so `PRP1` alone suffices — no PRP2 /
   PRP-list this slice. A multi-page request loops, advancing `buf_phys`/`lba`.
   `CDW10/11 = SLBA`, `CDW12 = nlb-1` (0-based). Throughput (one command per
   ≤page chunk) is a documented non-goal; DDR-767+ can add a PRP list.

3. **`blk_register`** — expose `nvme0` with `capacity_sectors = NSZE` and
   read/write thunks into `nvme_io`. **512-byte LBA only this slice** (QEMU's
   default; matches the block layer's 512-byte sector). If the namespace reports
   a non-512 LBA size, log and skip registration (documented non-goal — a
   sector-size shim is future work). Buffers are identity-mapped (phys==pointer),
   per `blk.h`.

4. **Self-test / gate** — `nvme_selftest()` (guarded so it only runs when an
   NVMe device is present) writes a known pattern to a scratch LBA on the
   `nvme.img` backing disk, reads it back through `nvme_io`, and verifies,
   printing `PRADYOS_NVME_RW_OK`. The `smoke-nvme` gate adds this to its
   `EXTRA_SENTINEL`. Only the dedicated scratch image is touched — never the
   boot/FAT/SFS disks.

## Non-goals (this slice)

- PRP lists / >page-boundary single commands (per-chunk loop instead).
- NVMe interrupts (still polling).
- Non-512-byte LBA sizes (skip-with-log).
- Multiple I/O queues / per-CPU queues; multiple namespaces/controllers.
- Mounting a filesystem on NVMe (that's a later integration slice once the block
  backend is proven).
