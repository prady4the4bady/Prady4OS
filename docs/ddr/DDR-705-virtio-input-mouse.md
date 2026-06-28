# DDR-705 — virtio-input pointer (mouse/tablet) → ring 3 (Layer 7)

> DDR before code, per the brief. Adds pointer input to the desktop: a virtio-input
> driver feeding a `SYS_MOUSE_POLL` syscall, and a cursor in the in-house compositor
> (DDR-704). With keyboard (DDR-703) + framebuffer (DDR-702/704) this completes the
> basic human-input surface.

## Decisions

### D1 — Device: virtio-input, absolute (tablet)
Target QEMU's **`virtio-tablet-pci`** (vendor `0x1AF4`, device `0x1052`, PCI base
class `0x09`). The tablet reports **absolute** axes (`EV_ABS` `ABS_X`/`ABS_Y`,
range 0..32767), which map directly to a screen cursor position — simpler and more
robust than relative (`virtio-mouse-pci`) for a compositor. Detected in kmain's
PCIe scan (`vendor 0x1AF4 && class 0x09`), like the GPU (class 0x03).

### D2 — Single event virtqueue, IRQ-driven, current-state model
Set up the **eventq** (queue 0) with N (32) writable 8-byte buffers; the device
writes one `virtio_input_event { le16 type, le16 code, le32 value }` per buffer and
returns it on the used ring. The IRQ handler (registered + INTx, like virtio-net)
pops completed events, updates a **current pointer state** (`abs_x`, `abs_y`,
`buttons`), and re-arms the buffer. `EV_ABS` updates the axis; `EV_KEY`
(`BTN_LEFT/RIGHT/MIDDLE`) sets/clears a button bit; `EV_SYN` is a frame boundary
(state already coherent). The statusq (queue 1, LEDs) is unused. Config-space
capability queries are skipped — the driver consumes events directly.

### D3 — `SYS_MOUSE_POLL` returns current state, non-consuming
```
SYS_MOUSE_POLL(struct mouse_state __user *) -> 0 | -ENODEV    [47]
struct mouse_state { int32 x, y; uint32 buttons; }
```
`x`,`y` are mapped to **screen pixels** in the kernel using the GPU framebuffer
geometry (`virtio_gpu_fb`): `x = abs_x * width / 32768`. Returns the latest state
(not a consuming queue), so the compositor and any other reader can both poll it
without stealing events (unlike the keyboard ring). `-ENODEV` if no pointer is up.

### D4 — Cursor in the compositor (DDR-704)
The compositor's loop also polls `SYS_MOUSE_POLL`; on a **button-down** it draws a
small cursor block at the pointer position, re-presents, and prints
`PRADYOS_MOUSE_OK x y` (event-driven — no continuous flushing). This shows the
pointer driving the desktop and provides the gate sentinel.

## Gate

`smoke-mouse` (CI, `QEMU_GPU=1` + `virtio-tablet-pci`): boot; wait for
`PRADYOS_COMPOSITOR_OK`; then inject an absolute move + a left-button press via
**QMP `input-send-event`** over a unix socket (a small python client does the QMP
`qmp_capabilities` handshake then the event) — the real virtio-input path. The
compositor reports `PRADYOS_MOUSE_OK <x> <y>`; the gate greps it.

## Non-goals (later)
Relative-mouse (`virtio-mouse-pci`) support, scroll wheel, cursor hotspot/shapes,
drag/hover semantics, multi-button chording, PS/2 fallback, and pointer routing to
per-client surfaces — all deferred. This is raw absolute pointer + buttons to ring
3 and a basic compositor cursor.
