= DDR-909 — item 24: USB 3.x xHCI host controller (design)

**Status:** Design. Governs the item-24 slice; written before its code.
**Date:** 2026-08-11

## Scope, and the gate that defines "done"

**Ship only on a proven end-to-end bulk transfer.** A line saying the controller
enumerated is not sufficient and must not be treated as such — that is the exact
"gate passes for the wrong reason" shape this project keeps finding (items 31,
39, 27). The gate reads a known byte pattern off a USB mass-storage device and
compares content, not call success.

Target: `qemu-system-x86_64 -device qemu-xhci -device usb-storage,drive=...`
with a backing file whose contents the gate knows.

Out of scope for this slice, and named so it is not silently assumed: isochronous
transfers, hubs beyond the root, USB 2.0 companion controllers, runtime PM,
hot-unplug during transfer.

## Register access — verify, do not trust this document

Every offset below must be checked against the **Eintel eXtensible Host Controller
Interface for USB rev 1.2** tables during implementation. They are recorded here
to fix the design's shape, not to be copied blind; a wrong constant taken from a
design doc is exactly the DDR-895 clamp mistake.

- Capability registers at BAR0+0: `CAPLENGTH` (0x00), `HCSPARAMS1` (0x04),
  `HCCPARAMS1` (0x10), `DBOFF` (0x14), `RTSOFF` (0x18).
- Operational registers at BAR0 + `CAPLENGTH`: `USBCMD`, `USBSTS`, `PAGESIZE`,
  `CRCR`, `DCBAAP`, `CONFIG`.
- Port registers at operational + 0x400 + 0x10*(port-1).
- Runtime registers at BAR0 + `RTSOFF`; interrupter 0 holds `ERSTSZ`, `ERSTBA`,
  `ERDP`.
- Doorbells at BAR0 + `DBOFF`, one dword per slot, slot 0 = command ring.

`MaxSlots` comes from `HCSPARAMS1`; `HCCPARAMS1.AC64` decides 32- vs 64-bit
addressing and `HCCPARAMS1.CSZ` decides whether contexts are 32 or 64 bytes.
**Context size is read, never assumed** — getting it wrong silently corrupts
every device context and presents as random enumeration failure.

## Bring-up order

1. **PCI discovery** — class 0x0C, subclass 0x03, prog-IF 0x30. Existing PCI
   enumeration is reused; no new scanner.
2. **BAR mapping** — BAR0 is MMIO, possibly 64-bit. Map uncached (`PCD`/`PWT`);
   xHCI registers must never be cached or the driver reads stale doorbell and
   status values. Bus-master and memory-space enable bits set in PCI command.
3. **USBLEGSUP handoff** — walk the xECP list from `HCCPARAMS1.xECP`. For
   capability ID 1, set the OS-owned semaphore, wait for the BIOS-owned
   semaphore to clear, then bounded-timeout and force it. **This is not
   optional**: without it the BIOS SMI handler keeps servicing the controller
   and fights the driver, producing failures that look like hardware faults.
4. **Reset** — `USBCMD.HCRST`, wait for `USBSTS.CNR` (controller-not-ready) to
   clear. All waits are bounded and fail loudly; no unbounded spin.
5. **DCBAA** — page-aligned array of 64-bit context pointers, `MaxSlots+1`
   entries, published in `DCBAAP`. Scratchpad buffers allocated if
   `HCSPARAMS2` asks for them, with the scratchpad array at DCBAA[0].
6. **Command ring** — physically contiguous TRB ring ending in a Link TRB with
   the Toggle Cycle bit; base and initial cycle state in `CRCR`.
7. **Event ring** — ERST with one segment; `ERSTSZ`, `ERSTBA`, `ERDP` in
   interrupter 0.
8. **Run** — `CONFIG.MaxSlotsEn`, then `USBCMD.RS`.

## Port and device flow

Root port reset via `PORTSC.PR`, wait for `PRC`. Then:
`Enable Slot` → allocate Input Context → `Address Device` → `GET_DESCRIPTOR`
on the default control pipe → `Configure Endpoint` for the bulk IN/OUT pair →
bulk transfer.

USB 3 ports report `PORTSC.PED` set after reset without an explicit enable; USB 2
ports do not. The driver branches on the port's protocol from the Supported
Protocol xECP rather than on port number.

## Concurrency and the cycle bit

The single subtle correctness point: **producer and consumer cycle state**. The
driver writes TRBs with the current cycle bit and the controller consumes them
when the bit matches its own; both toggle at the Link TRB. An off-by-one here
produces a controller that appears hung with no error — indistinguishable from a
missing doorbell.

The event-ring dequeue pointer must be written back to `ERDP` with the Event
Handler Busy bit cleared, or interrupts stop after the first event.

Ring memory is allocated from the PMM (per the low-mem rule: large tables never
from BSS) and must be physically contiguous and 64-byte aligned.

## Interrupts

INTx is shared, so the handler chains (per the kernel's existing `idt.c`
convention) and must check `USBSTS.EINT` before claiming the interrupt. MSI/MSI-X
is not attempted in this slice; the reason is recorded rather than left implicit:
INTx is sufficient for the gate and adds one fewer moving part to a driver whose
first correctness proof matters more than its latency.

## Failure policy

Every wait is bounded. Every bounded wait that expires logs the register it was
waiting on and its last value, then fails the operation — no silent continue.
A driver that proceeds past a failed reset produces symptoms attributable to
anything, which is the debugging cost this project has repeatedly paid.
