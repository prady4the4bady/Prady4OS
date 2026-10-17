# DDR-1164 — USB piece 4: port reset, configuration descriptor, SET_CONFIGURATION

Status: design committed before code (NON-NEGOTIABLE 5). Stacked on DDR-1163 (PR #39).

## 1. What this adds

DDR-1163 addressed the first connected device and read its device descriptor.
It sent Address Device without resetting the port first. QEMU accepts that,
but real hardware does not: xHCI §4.3.1 requires a root-hub port reset (PR) so
that the port reaches the Enabled state. This piece adds three steps:

1. **Port reset.** Write `PORTSC = PP | PR`. The register's change bits are
   write-1-to-clear, and PED is write-1-to-disable, so writing back a value read
   from the register would clear or disable things by accident. Then poll for
   PRC (bit 21), clear it by writing `PP | PRC`, and read PED (bit 1).
   This prints `[xhci] port-reset port=N ped0=X ped=1`. `ped0` is PED before the
   reset. It is measured, not assumed: on a USB2 port PED reads 0 until the
   reset completes.
2. **Configuration descriptor.** Send GET_DESCRIPTOR(CONFIGURATION) for 9 bytes
   to read wTotalLength, then fetch the whole descriptor (capped at 256 B).
   Walk the descriptors and pick the first interface plus the first endpoint
   that follows it. This prints `[xhci] config-desc cc= total= cfg= ifaces=
   class= sub= proto= ep= attr= mps= ival=`.
3. **SET_CONFIGURATION, then GET_CONFIGURATION to read it back.** This prints
   `[xhci] set-config cfg=N cc=1 got=N`. The readback value `got=` comes from
   the device, so it is evidence the request took effect. Printing the value the
   driver *sent* would prove nothing.

EP0 control transfers become a helper, `control(slot, setup, buf_p, len, in)`.
The EP0 transfer ring and its cursor move into `X`, so they persist across
requests. A no-data request uses a Setup TRB with TRT=0 and an IN Status stage.

## 2. Gate

`smoke-xhci` gains three exact required lines. They are measured on the
QEMU `usb-kbd`: HID boot keyboard, class 3/1/1, interrupt IN endpoint 0x81.

## 3. Mutants (planned)

- **M1:** SET_CONFIGURATION with wValue 0 (unconfigure). The device then reads
  back `got=0`, so the set-config line fails. This proves `got=` comes from the
  device.
- **M2:** read only the 9-byte header and skip the full fetch. The interface and
  endpoint fields read 0, so the config-desc line fails.

## 4. Not done

- Endpoint configuration (Configure Endpoint command) and interrupt-IN polling
  are not done; HID reports are piece 5.
- Only the first connected port is handled. There is no hub support and no
  hot-plug.
- The reset matters on real hardware but only its *effect* (PED, PRC) is
  measured here, under QEMU.

## 7. Measured

Shipped kernel `a5c8bb5b6bd2a37c`, 1,479,050 B. Code was added, but the size
is unchanged: it fits in existing page padding, so the size/headroom pair does
not move. QEMU `usb-kbd` on port 5:

    [xhci] port-reset port=5 ped0=0 ped=1
    [xhci] config-desc cc=1 total=34 cfg=1 ifaces=1 class=3 sub=1 proto=1 ep=0x0000000000000081 attr=3 mps=8 ival=7
    [xhci] set-config cfg=1 cc=1 got=1

`ped0=0` means the USB2 port really is disabled before the reset, so the reset
is load-bearing on this path and not ceremonial.

| run | kernel | smoke-xhci | failing line |
|---|---|---|---|
| shipped | `a5c8bb5b6bd2a37c` | rc=0 | — |
| M1 (SET_CONFIGURATION value 0) | `d3ca5b5fe6ed45c7` | rc=2 | set-config (device read back `got=0`) |
| M2 (no full fetch) | `03b06b351c266046` | rc=2 | config-desc |
| revert | `a5c8bb5b6bd2a37c` | rc=0 | bit-for-bit |

`smoke-shell` and `smoke-mouse` are rc=0. The hygiene checks are all rc=0.
GLOBAL_FORBIDDEN and the gate count are unchanged. This is emulated qemu-xhci
only.
