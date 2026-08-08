= DDR-875 — AHCI (SATA) driver (Group 4 item 23)

**Status:** Accepted
**Date:** 2026-08-09
**Scope:** `kernel/drivers/ahci/`, `kernel/main.c`, `boot_test.sh`, `smoke-ahci`.

## Scope, stated before the detail

FIS-based DMA read and write on ports the HBA reports as carrying a SATA disk,
each registered as a `blk_device` so SFS/FAT/ext4 mount on it exactly as they do
on virtio-blk.

**Not implemented:** NCQ (queued commands), port multipliers, ATAPI, hot-plug.
These are real AHCI features and their absence is a scope decision, not an
oversight — nothing in this queue needs them, and each adds a failure mode
needing its own gate.

## The three things that make AHCI subtly wrong

**1. Stop before you touch.** A port's command-list and FIS-base registers may
only be written while the engine is stopped **and has acknowledged it**
(`PxCMD.CR = 0`, `PxCMD.FR = 0`). Writing them on a running port is undefined:
the HBA may keep using the old addresses, so the driver reads a buffer the disk
never wrote and calls it data. Every setup path stops and *waits*.

**2. The alignment requirements fail silently.** The command list must be 1 KiB
aligned and the FIS area 256-byte aligned. The registers simply **ignore the low
bits** — a misaligned allocation does not error, it points the HBA somewhere
other than where the driver thinks. Both areas come from whole PMM pages, which
satisfies both bounds by construction.

**3. `dbc` is a byte count MINUS ONE.** Writing the plain count transfers one
byte too many — for a 512-byte sector that overruns the caller's buffer by
exactly one byte, damage that surfaces far from here.

Also: **bus-master enable**. Without setting it in the PCI command register the
HBA cannot DMA at all, every command times out, and the disk looks broken rather
than unconfigured.

And the PCI match is on class **0x01 AND subclass 0x06**. Class 0x01 alone is
"mass storage" and would also catch IDE and NVMe controllers this driver cannot
drive.

Every hardware poll is **bounded**. An unbounded `while (reg & BUSY)` on a
wedged controller hangs boot with no message, indistinguishable from a hang
anywhere else.

## The gate attaches a real disk

`smoke-ahci` adds an `ich9-ahci` controller with an 8 MiB SATA drive and
requires:

```
[ahci] port disk, sectors=16384
```

16384 × 512 = exactly 8 MiB, so the assertion proves **IDENTIFY returned real
data** rather than the driver merely finding a controller. A sector count that
matched anything else would be a fabricated capacity, and a wrong capacity lets
the filesystem address past the end of the disk.

The FORBIDDEN sentinels (`IDENTIFY failed`, `port would not stop`, `cannot map
ABAR`) catch the failure that would otherwise read as success: a controller
found, but whose port setup quietly failed, still prints a "controller ready"
line.

**The device is opt-in** (`QEMU_AHCI_IMG`). Always-on would change PCI
enumeration for every gate, and a driver probing a device that is only sometimes
present is exactly how an intermittent boot difference appears.

## What is proven, and what is not

**Proven:** PCI probe → ABAR map → HBA enable → port stop/start → command list
and FIS setup → IDENTIFY via DMA → capacity → `blk_register`. IDENTIFY is a real
DMA transfer through the same `issue()` path reads use, so DMA-in works.

**Not yet proven: DMA write.** It uses the same machinery with the direction bit
set, but no gate has written a sector and read it back. Stated rather than
implied — an untested write path in a block driver is exactly the thing that
should not be assumed working. A read-back gate is the natural next step, and
would pair with item 30 if SFS ever roots on a SATA disk.

Gates green: `smoke-ahci`, `smoke`, `smoke-fs-sfs-rw`. Zero warnings under
`-Werror`. 137 gates across 6 shards.

**Group 4 item 23 complete for the read path; write path implemented but ungated.**
