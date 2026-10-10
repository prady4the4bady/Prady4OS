# DDR-1161 — USB piece 1: xHCI controller bring-up and port detection

Status: design (committed before code, NON-NEGOTIABLE 5). Operator order: PR #27
comment 5976228741 Step 3 — "USB first: controller (xHCI), hubs, HID keyboard
and mouse, mass storage". This DDR is the **first piece only**.

## §1 What exists

Nothing. `grep -ril usb kernel/` returns no file (checked 2026-10-10). QEMU 8.2
provides `qemu-xhci` with `usb-kbd`, `usb-mouse`, `usb-tablet` (VERIFIED in
PR #27 comment 5968403697).

## §2 Scope of this piece

1. Match the controller in the PCI loop: class 0x0C, subclass 0x03, prog_if 0x30.
2. Enable memory + bus master, map BAR0 (64-bit aware) into a new uncached VA
   window `XHCI_BAR_VBASE = 0xFFFFD60000000000` (clear of ECAM, virtio, NVMe, GOP).
3. Read the capability registers: CAPLENGTH, HCIVERSION, HCSPARAMS1
   (MaxSlots, MaxPorts).
4. Halt (USBCMD.RS=0, wait USBSTS.HCH=1), reset (USBCMD.HCRST=1, wait HCRST=0
   and USBSTS.CNR=0), all with bounded waits.
5. Walk PORTSC for each port (operational base + 0x400 + 0x10·(n−1)) and print
   connected ports with their speed field.

Out of scope here (later pieces, each its own DDR): command/event rings, slot
enable, device addressing, descriptors, HID class driver, hubs, mass storage,
interrupts.

## §3 Output

```
[xhci] up hciver=0x0100 slots=64 ports=8
[xhci] port=5 ccs=1 speed=1
[xhci] port=6 ccs=1 speed=1
[xhci] connected=2
```
(exact values measured and recorded in §6). Failures print `[xhci] FAIL <step>`.

## §4 Gate — `smoke-xhci`

`boot_test.sh` gains `QEMU_XHCI=1`, which attaches `-device qemu-xhci` plus
`usb-kbd` and `usb-tablet`. The gate requires the `up` line and `connected=2`,
and forbids `[xhci] FAIL`. Every other gate boots without a controller and the
driver is a no-op.

## §5 Mutation check

- **M1**: ignore CAPLENGTH (treat the operational base as BAR0). PORTSC is then
  read from the wrong offset and `connected=2` is not printed.
- **M2**: test the wrong PORTSC bit for CCS. `connected=` changes.

## §6 What is and is not proven

Proves: on QEMU's emulated `qemu-xhci`, the kernel finds the controller, resets
it and sees exactly the two attached devices. Does NOT prove anything about real
hardware controllers (the operator's Legion or VMware's xHCI), and no USB device
is usable yet: nothing is enumerated or addressed. USB is still ABSENT as a
user-visible feature after this piece.

## §7 DDR number

1161: no DDR-1161 on any remote branch (checked 2026-10-10). 1159 = Fix 3,
1160 = Fix 4.

## §8 Measured

Shipped kernel `6cf070a520e86b32` (1,474,954 B, size unchanged). `smoke-xhci` rc=0:

```
[xhci] up hciver=0x0000000000000100 slots=64 ports=8
[xhci] port=5 ccs=1 speed=3
[xhci] port=6 ccs=1 speed=3
[xhci] connected=2
```

Speed 3 = high-speed: QEMU places `usb-kbd`/`usb-tablet` on the USB2 ports (5, 6).
The gate asserts all four lines exactly.

A correction found while building: HCIVERSION first read **0**. QEMU's
capability region returns 0 for a 16-bit read at offset 2, so CAPLENGTH and
HCIVERSION are read together as dword 0.

| mutant | hash | result |
|---|---|---|
| M1 (operational base = BAR0, CAPLENGTH ignored) | `170ebf9c2e6c5081` | rc=2, `[xhci] FAIL halt` |
| M2 (CCS tested at bit 1) | `b8f14edb47eed414` | rc=2, `connected=0` |

§5 predicted M1 would fail at the PORTSC read. It fails earlier: USBCMD/USBSTS
are also operational registers, so the halt wait never sees HCH. Either way the
gate catches it. Revert returns `6cf070a520e86b32` bit-for-bit.

Regression, one QEMU at a time, all rc=0: `smoke-shell`, `smoke-nvme`,
`smoke-chipset`, `smoke-mouse`. Hygiene: ALL TEN PASSED. `ci-shard-check`: 187
gates (`smoke-xhci`, shard 4, strict).
