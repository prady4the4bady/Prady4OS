# DDR-1162 — USB piece 2: xHCI command/event rings and Enable Slot

Status: design (committed before code, NON-NEGOTIABLE 5). Builds on DDR-1161
(piece 1, PR #37). Operator order: PR #27, USB first.

## §1 Scope

After DDR-1161's reset, and before setting USBCMD.RS:

1. Program CONFIG.MaxSlotsEn = HCSPARAMS1.MaxSlots.
2. Allocate the DCBAA (zeroed, 64-byte aligned, (MaxSlots+1)×8 B) and write DCBAAP.
   If HCSPARAMS2 asks for scratchpad buffers, allocate the array and the pages and
   put the array at DCBAA[0].
3. Command ring: one page of TRBs, with a Link TRB at the end (TC=1). Write CRCR
   with RCS=1.
4. Event ring for interrupter 0: one page of TRBs plus a one-entry ERST. Write
   ERSTSZ=1, ERDP, then ERSTBA (in that order, per the spec).
5. Set USBCMD.RS=1 and wait (bounded) for USBSTS.HCH=0.
6. Issue an Enable Slot command: put the TRB on the command ring and ring
   doorbell 0. **Poll** the event ring (no interrupts in this piece) for a Command
   Completion Event. Check the completion code is Success (1), read the slot ID,
   and advance ERDP with EHB set.

Out of scope here: Address Device, input contexts, descriptors, interrupts/MSI-X,
HID, hubs, mass storage.

## §2 Output

```
[xhci] running
[xhci] enable-slot cc=1 slot=1
```

A failure prints `[xhci] FAIL <step>`. That string is forbidden by `smoke-xhci`.

## §3 Gate

`smoke-xhci` adds the two lines above as exact required patterns. The piece-1
lines are kept. No new gate is added.

## §4 Mutation check

- **M1**: do not write CRCR. The controller has no command ring and never posts
  a completion event, so the bounded poll times out (`FAIL enable-slot`).
- **M2**: start the event ring's consumer cycle bit at 0 instead of 1. The poll
  never sees a valid event, so it also times out — but at a different step: the
  command reaches the controller and the fault is in event consumption. The
  failure message names the step so M1 and M2 are distinguishable.

## §5 What is and is not proven

This proves that on QEMU's emulated `qemu-xhci`, the command and event rings
work end to end: the controller fetches a command TRB and returns a completion
event that the driver reads correctly. It proves nothing about real
controllers. No device is addressed, so USB is still not usable as a feature.

## §6 DDR number

1162 has no file in `docs/ddr/` or `docs/decisions/`, and no remote branch
names it (checked 2026-10-11).

## §7 Measured

(filled after build)
