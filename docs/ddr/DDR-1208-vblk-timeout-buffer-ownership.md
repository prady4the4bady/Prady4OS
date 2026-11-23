# DDR-1208: a timed-out virtio-blk request keeps its memory with the device

Status: design, then code in the same PR. Found reviewing #145 (CodeRabbit on `kernel/syscall/sys_thread.c`). Done in this session on the operator's request. QEMU only, not verified on real hardware.

## 1. The defect, read from the code

- `submit()` (`kernel/drivers/blk/virtio_blk.c`) waits 500 ticks for a data request. On timeout it calls `data_timeout_release()` and returns `-EIO`.
- DDR-1160 made that release safe for **wake-ups**. It unmaps `head2slot[head]`, so a late completion of the old head cannot wake the slot's next owner.
- It did not make it safe for **memory**. The device still owns the descriptor chain, and the chain points at two things that are reused at once:
  1. **The caller's data buffer.** `vblk_read`/`vblk_write` pass the caller's pointer as the DMA address. Every caller frees, reuses or returns that buffer after `-EIO`:
     - `sys_disk_list` (NSI 104) frees its sector page;
     - `SYS_SCHED_CPU` op 5 freed its 64 KiB buffer (#145 now leaks it instead);
     - `sfs`, `fat32` and `ext4` read into scratch and cache buffers that they reuse;
     - `install.c` and the boot self-tests read into their own pages.
     A late read then writes into memory that now belongs to someone else. A late write puts whatever now lives there on disk.
  2. **The slot's own header and status byte** (`reqbuf + s*32`). The release sets `used = 0`, so the next request takes the slot and rewrites the header. The device can still write the old request's status byte over the new one.
- No caller can be fixed one at a time. The FS layers pass buffers the driver cannot see the lifetime of, and a stack or cache buffer cannot be "leaked".

## 2. Decision

**D1. Driver-owned DMA memory (bounce buffers).**
- Each slot owns a 32 KiB bounce buffer (64 sectors), allocated with the slot at probe (`pmm_alloc_pages(3)` per slot; 8 slots, 256 KiB per device).
- `submit()` takes at most 64 sectors:
  - a write copies the caller's data into the bounce before the request is published;
  - a read copies the bounce into the caller's buffer after a normal completion.
- `vblk_read`/`vblk_write` split larger requests into 64-sector pieces.
- The device never holds a pointer to caller memory, so a caller may free its buffer after any return, `-EIO` included.
- **Cost:** one memory copy per request, and up to 2 MiB with eight disks out of the 512 MiB gate VM.
- **Failure:** if a bounce cannot be allocated, the device is not registered and the probe prints why, as for `reqbuf`.

**D2. A timed-out slot is quarantined, not released.**
- `data_timeout_release()` keeps `used = 1`, marks the slot `orphan`, and leaves `head2slot[head]` mapped. It drops the waiter. It no longer wakes a slot waiter, as DDR-1160's release did, because no slot is free yet.
- The slot's header, status byte and bounce stay with the device.
- When the late completion arrives, `reap_head()` finds the orphan through its mapping. It frees the slot (`used = 0`, `orphan = 0`, wakes one slot waiter) and wakes no request. It still counts the completion as `late` (DDR-1148).
- **The head cannot be reused meanwhile.** The chain's descriptors are freed only when the late completion is reaped (`complete()` calls `virtq_free_chain()` after `reap_head()`). DDR-1160's guarantee therefore holds by construction: nobody else owns the slot until the old head is gone.
- **Limit, stated:** a device that never completes an orphaned request keeps that slot forever. After 8 such timeouts every submitter's slot wait times out (`-EIO`), which is how a hung device already fails today.

**D2a. With P5's I/O pressure (#145).** P5 added a per-request flag `req[s].psi`, lowered once by the completion or the timeout, whichever comes first. The timeout path lowers it before the slot is held, so an orphan's late completion finds it already 0 and lowers nothing. Exactly once still holds. P5's note that "a late completion finds no slot" no longer applies: it now finds the held slot, with the flag already cleared.

**D3. Callers do not change.** Op 5's leak-on-`-EIO` from #145 becomes unnecessary but stays harmless, and it can be removed later.

**Not taken:**
- **A device reset on timeout.** That would cancel every in-flight request on the device, and re-initialising the queue under load touches DDR-1138's lock-owner bookkeeping. A larger change for the same guarantee.
- **Per-caller fixes.** See §1: not every buffer can be identified, let alone leaked.

## 3. Gate: `smoke-vblklate`, two arms

- **Arm Q (quarantine), extended from DDR-1160's self-test** on a scratch `struct vblk` (no device):
  1. A slot with head 7 times out (`data_timeout_release`). Assert the slot is still held (`held=1`).
  2. The late completion of head 7 is reaped. Assert it woke nothing (`done_stale=0`), was counted (`late=1`) and freed the slot (`freed=1`).
  3. A new owner with head 9 takes the slot and completes. Assert `done_real=1`.
  - **Mutant Q1:** `data_timeout_release` sets `used = 0` again, which is today's behaviour. That gives `held=0`, so the arm is red.
- **Arm B (bounce), on the real unit 0:**
  1. Under `compl_lock`, fill the bounce of every slot not in use with `0xA5`.
  2. Read sector 0 (the boot sector, signature `55 AA`) into a fresh page with `blk_read`.
  3. Assert that the page holds the signature, and that some slot's bounce holds the same 512 bytes (`via=1`).
  - **Mutant B1:** the read DMAs straight into the caller's page, the pre-fix path. Every bounce keeps its `0xA5` fill, so `via=0` and the arm is red.
  - Filling only unused slots is safe: a slot's next request overwrites its bounce before reading it (write) or by DMA (read).
- **Vacuity, read before writing:**
  - On today's tree, Q's `held` reads 0, because the release frees the slot.
  - B's bounce path does not exist on today's tree; mutant B1 is that tree's behaviour.
  - Each mutant is red on its own arm only: Q1 does not touch the data path, and B1 does not touch the timeout path.
- **New sentinel:** `PRADYOS_VBLKLATE_OK held=1 done_stale=0 late=1 freed=1 done_real=1` and `PRADYOS_VBLKBOUNCE via=1 sig=1`.

**Not gated (stated):**
- a real 5 s data stall followed by a late completion (QEMU cannot produce one on demand, as DDR-1160 §3 says);
- the split of requests over 64 sectors (op 5 and the installer exercise it in other gates, but no arm asserts it);
- all eight slots orphaned;
- the probe failure when a bounce cannot be allocated.

## 4. Risk

The data path of every disk changes: every request now copies through a bounce, and large requests are split. Every PR runs the whole gate set, and the FS, installer and partition gates exercise the path. A new `[vblkto]`, hang or data-compare failure is a stop. The completion path (`reap_head`, IRQ context, `compl_lock`) gains one branch for orphans and no new lock.

QEMU (TCG, 4 vCPU) only. Not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1–U3.
