# DDR-1163 — USB piece 3: Address Device and the device descriptor

Status: design (committed before code, NON-NEGOTIABLE 5). Builds on DDR-1162
(piece 2, PR #38). Operator order: PR #27, USB first.

## §1 Scope

For the first connected port that DDR-1161 found (on the QEMU gate this is
port 5, a usb-kbd, high speed):

1. Allocate an output device context and an input context. The context size
   (32 or 64 bytes) comes from HCCPARAMS1.CSZ. Put the output context in
   DCBAA[slot].
2. Allocate a transfer ring for EP0 (one page, with a Link TRB at the end).
3. Fill the input context: add flags A0|A1; a slot context with speed, root-hub
   port and Context Entries = 1; an EP0 context with type Control, CErr 3, max
   packet size chosen from the port speed (SS 512, HS 64, FS/LS 8), the
   dequeue pointer with DCS = 1, and average TRB length 8.
4. Send an Address Device command (TRB type 11) and wait for its completion.
5. On EP0, send GET_DESCRIPTOR(DEVICE, 18 bytes): a Setup TRB (IDT, TRT = IN),
   a Data TRB (IN, 18 bytes into a DMA page), and a Status TRB (OUT, IOC). Ring
   doorbell `slot` with target 1. Wait for the Transfer Event.

Command submission and event consumption become helpers that keep a ring index
and a cycle bit, so piece 2's Enable Slot uses the same code. Polling only; no
interrupts.

## §2 Output

```
[xhci] address port=5 slot=1 cc=1
[xhci] device-desc cc=1 len=18 type=1 usb=0x0000000000000200 mps0=64 vid=0x0000000000000627 pid=0x0000000000000001
```

The exact values on the last line are the ones measured on QEMU's usb-kbd and
are recorded in §7. A failure prints `[xhci] FAIL <step>`.

## §3 Gate

`smoke-xhci` adds both lines as exact required patterns. No new gate.

## §4 Mutation check

- **M1**: set the slot context's root-hub port number to 0. The controller
  should reject Address Device, which prints `FAIL address`.
- **M2**: drop IDT from the Setup TRB. The controller then treats the request
  bytes as a pointer, so the descriptor read fails or returns the wrong bytes,
  and the exact `device-desc` line does not appear.

## §5 What is and is not proven

This shows that QEMU's emulated `qemu-xhci` addresses one emulated device, and
that a control transfer on EP0 returns the 18-byte device descriptor. It proves
nothing about real controllers or real devices. It does not cover SET_CONFIGURATION,
the configuration descriptor, interrupt endpoints, HID reports, hubs or mass
storage. USB is still not usable as a feature.

## §6 DDR number

1163 has no file in `docs/ddr/` or `docs/decisions/`, and no remote branch
names it (checked 2026-10-11).

## §7 Measured

Shipped kernel `a87aed047055e1f0`, 1,479,050 B, which is +4,096 B (one page). The size/headroom pair is updated in `CLAUDE.md` and in `PRE_LAUNCH_CHECKLIST` §6. `smoke-xhci` passes (rc=0) with all eight exact lines. The descriptor matches QEMU's usb-kbd: USB 2.00, mps0 64, vid 0x0627, pid 0x0001.

| mutant | hash | result |
|---|---|---|
| M1 (slot root-hub port = 0) | `3f7db42031008147` | rc=2, `[xhci] FAIL address` |
| M2 (Setup TRB without IDT) | `8fc61ec8d7fa40af` | rc=2, `[xhci] FAIL desc` (no Transfer Event) |

The two mutants fail at different steps. Reverting either one gives back `a87aed047055e1f0` bit-for-bit.

Ports were not reset before Address Device. QEMU accepts this for a USB2 port. Real hardware generally requires a port reset first, so a real-hardware port of this code must add one. This is recorded here and was not tested.

Regression, one QEMU at a time, all rc=0: `smoke-shell`, `smoke-mouse`, `smoke-nvme`. Hygiene: ALL TEN PASSED.
