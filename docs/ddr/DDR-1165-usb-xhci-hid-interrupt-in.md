# DDR-1165 — USB piece 5: Configure Endpoint and one HID boot report

Status: design committed before code (NON-NEGOTIABLE 5). Stacked on DDR-1164 (PR #40).

## 1. What this adds

1. **Configure Endpoint** (command TRB type 12) for the interrupt-IN endpoint
   that DDR-1164 found: 0x81, MPS 8, bInterval 7 on a high-speed port.
   - DCI = 2·1 + 1 = **3**. The input context sets add flags A0 | A3, and the
     slot context's Context Entries field becomes 3.
   - EP context: CErr 3, EP type 7 (Interrupt IN), MPS 8,
     Interval = bInterval − 1 = 6 (2⁶ × 125 µs), average TRB length 8,
     Max ESIT payload 8, and its own transfer ring with DCS = 1.
   - Prints `[xhci] configure-ep ep=0x81 dci=3 cc=1`.
2. **SET_PROTOCOL(boot)**: class request 0x21/0x0B, wValue 0, wIndex =
   interface number. Prints `[xhci] set-protocol cc=1`. Boot protocol fixes the
   report format at 8 bytes: modifier, reserved, keys[6].
3. **One report**, run only when the `usbkbd` fw_cfg probe key is set:
   - Queue a Normal TRB (8 B, IOC) and ring doorbell target 3.
   - Print `[xhci] hid-armed`. The injector waits for this line.
   - Wait for Transfer Events on endpoint 3. A report whose key slot is empty
     (a release, or idle) is re-queued, up to 20 tries.
   - Print `[xhci] hid-report cc=1 mod=0 k0=4`. Usage 4 is HID `a`.

Without the probe key, nothing waits. Every other boot pays only the configure
and set-protocol commands; `smoke-xhci` gains those two lines.

## 2. Gate

New gate `smoke-usbkbd` (shard 4, strict). It boots QEMU directly, the
`smoke-modkeys` shape, with `qemu-xhci` + `usb-kbd` + an HMP monitor and probe
`usbkbd`, and runs `input_inject.sh` with sentinel `[xhci] hid-armed` and key
`a`.

It requires the hid-report line exactly. The value `k0=4` comes from the
device, after a host-side keypress, so the gate cannot pass on a driver that
only prints. That only holds if QEMU routes `sendkey` to the usb-kbd and not to
the i8042; this is measured, not assumed (§5).

## 3. Mutants (planned)

- **M1:** EP type 3 (Interrupt OUT) instead of 7. No IN data reaches the ring,
  so the hid-report line is missing.
- **M2:** read k0 from byte 0 (the modifier byte) instead of byte 2. This prints
  `k0=0`, so the exact line fails. It proves the key code is read from the
  report rather than being an incidental non-zero.

## 4. Not done

- One report only: no continuous polling, no keymap, no input-layer
  integration.
- No release handling beyond skipping empty reports.
- Emulated qemu-xhci only.

## 5. Measured

Shipped kernel `d8d48f39c25e0ff9`, 1,483,146 B (+4,096 B; the size/headroom
pair is recomputed at both places it is recorded: 89,718 B headroom).

    [xhci] configure-ep ep=0x0000000000000081 dci=3 cc=1
    [xhci] set-protocol cc=1
    [xhci] hid-armed
    [xhci] hid-report cc=1 mod=0 k0=4

**Routing, measured.** With `usb-kbd` attached, QEMU delivered the HMP
`sendkey a` to the USB keyboard: `k0=4` is HID usage `a`, and the i8042 path
cannot produce it.

| run | kernel | smoke-usbkbd | failing line |
|---|---|---|---|
| shipped | `d8d48f39c25e0ff9` | rc=0 (run twice) | — |
| M1 (EP type 3, Interrupt OUT) | `7937a5186ab12528` | rc=2 | hid-report |
| M2 (k0 from byte 0) | `e7836c1f394d6563` | rc=2 | hid-report |
| revert | `d8d48f39c25e0ff9` | rc=0 | bit-for-bit |

Other gates:
- `smoke-xhci` gains the configure-ep and set-protocol lines and is rc=0.
- `smoke-shell`, `smoke-mouse` and `smoke-nvme` are rc=0.
- Hygiene: all ten checks rc=0, docstate OK.
- `ci-shard-check`: 188 gates (`smoke-usbkbd` on shard 4, strict).

Before the first gate run, `pgrep` showed one other QEMU (PID 1485). It had
exited by the time I checked, most likely the tail of the previous gate's
injector-driven boot. The gate was then rerun with no other QEMU present.
