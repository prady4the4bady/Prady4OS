# DDR-1160 — virtio-blk: a late completion must not wake the slot's new owner (Fix 4, audit F1)

Status: design (committed before code, NON-NEGOTIABLE 5). Approved as Fix 4 in
PR #27 comment 5977642031. Audit source: `docs/AUDIT_LIFETIME_2026-10.md` F1.

## §1 Defect (from source, `kernel/drivers/blk/virtio_blk.c` on `dev/phase1`)

`submit()` maps the used-ring head to the slot: `v->head2slot[head] = s`.
On a **data-wait timeout** (500 ticks, the `[vblk] compl wait timeout` path) it
releases the slot — `req[s].used = 0; req[s].waiter = 0; slot_wake_one(v)` —
and returns `-EIO`, but it **does not clear `head2slot[head]`**, and the
descriptor chain is still owned by the device.

The slot can now be reused by another submitter, which sets `req[s].done = 0`,
its own `waiter`, and gets a NEW head `H2`. If the device then completes the
OLD head `H`, `complete()` reads `s = head2slot[H]` (still `s`), counts it as
`late` (DDR-1148: `!used || head != H`), **and still sets `req[s].done = 1` and
wakes `req[s].waiter`** — the new owner. That submitter returns as if its own
request finished: status byte not yet written by the device for `H2`, and its
data buffer not yet filled (read) or not yet on disk (write).

Reach: only after a 5 s completion timeout, which is itself a failure. The
consequence is a silent wrong result on an unrelated, later request.

## §2 Fix

On the data-wait timeout path, under `compl_lock` (already held), clear
`v->head2slot[v->req[s].head] = -1` before releasing the slot. A late completion
of `H` then maps to no slot: `complete()` frees the chain (the device has
finished with it) and wakes nobody.

The DDR-1148 `late=` counter is **kept**: `complete()` counts a reaped head
that maps to no slot (`s < 0`) as `late`, in addition to the existing
`!used || head != head` test. So a late completion is still counted, now on
the `s < 0` branch.

To make this testable without a real 5 s device stall, the per-head body of
`complete()` is factored into `reap_head(v, head)` (no ring access) and the
timeout release into `data_timeout_release(v, s)`. Behaviour of the healthy
path is unchanged.

## §3 Gate — `smoke-vblklate` (new)

No existing gate can force a data-wait timeout followed by a late completion:
QEMU's virtio-blk does not stall on demand. So the gate drives the **driver
logic** directly: `virtio_blk_late_selftest()` (under probe `vblklate`) builds a
scratch `struct vblk` (no device), and:

1. slot 0 submitted with head 7 (`head2slot[7] = 0`), then `data_timeout_release(v, 0)`;
2. slot 0 reused with head 9, `done = 0`, a non-zero waiter sentinel is not
   used — `waiter = 0` so no real thread is unblocked;
3. `reap_head(v, 7)` — the late completion of the old head;
4. asserts `req[0].done == 0` (new owner NOT woken) and `late == 1`;
5. `reap_head(v, 9)` — the real completion — asserts `done == 1`, `late == 1`.

Prints `PRADYOS_VBLKLATE_OK done_stale=0 late=1 done_real=1`; any failure
prints `VBLKLATE FAIL: ...` (forbidden).

Arm 5 is the non-vacuity control: a `reap_head` that never sets `done` would
pass arm 4.

## §4 Mutation check

- **M1**: drop the `head2slot` clear in `data_timeout_release`. Arm 4 must fail
  (`done_stale=1`). This is the defect itself.
- **M2**: drop the `s < 0` `late++`. Arm 4 must fail on `late=0`. Proves the
  counter is kept.

## §5 What the gate does and does not prove

Proves: the driver's slot/head bookkeeping no longer lets a stale head wake a
reused slot, and still counts it. Does **not** prove anything about a real
device: no real late completion is produced, and the DMA hazard (the device may
still write into the timed-out caller's buffer after `-EIO`) is **not** addressed
here — it is the same class as audit F3, which is not approved. Does not close
U1, U2, U3, OPEN-1 or OPEN-2.

## §6 DDR number

1160: `ls docs/ddr docs/decisions` on every remote branch shows no DDR-116x
(checked 2026-10-10). 1159 is Fix 3 (PR #34).

## §7 Measured

| kernel | hash | `smoke-vblklate` |
|---|---|---|
| shipped Fix 4 | `ebd9bdd8f9541e9d` (1,474,954 B, size unchanged from `dev/phase1`) | rc=0, `PRADYOS_VBLKLATE_OK done_stale=0 late=1 done_real=1` |
| M1 (no `head2slot` clear) | `4bf3175b0eabc6eb` | rc=2, `VBLKLATE FAIL: done_stale=1 late=1 done_real=1` — the defect reproduced in the driver logic |
| M2 (no `s<0` `late++`) | `c085ad5e6669e0c9` | rc=2, `VBLKLATE FAIL: done_stale=0 late=0 done_real=1` |

Revert returns `ebd9bdd8f9541e9d` bit-for-bit. Note M1's `late=1`: before the
fix the stale completion WAS counted (DDR-1148) and still woke the new owner —
exactly "counted, not fixed". `ci-shard-check`: 187 gates (186 + `smoke-vblklate`,
shard 1, strict).

Regression on `ebd9bdd8f9541e9d`, one QEMU at a time, all rc=0: `smoke-vblklate`,
`smoke-blk-integrity`, `smoke-blkmq`, `smoke-blk-timeout`, `smoke-part`,
`smoke-shell` (once). `hygiene_check.sh`: ALL TEN PASSED.
