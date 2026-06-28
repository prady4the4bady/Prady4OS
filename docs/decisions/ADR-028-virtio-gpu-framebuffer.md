# ADR-028: VirtIO-GPU framebuffer (Layer-7 slice 0)

- **Status:** Accepted 2026-06-28 — design record for the GPU framebuffer slice.
- **Date:** 2026-06-28
- **Phase:** Layer 7 slice 0 — the framebuffer prerequisite the UI/UX brief
  requires before its compositor build order (§12 7a–7j) can begin.
- **Relation to prior ADRs:** builds on **ADR-013** (PCIe), **ADR-014** (virtio-blk
  transport) and the shared `virtio_pci`/`virtq` layer reused by virtio-blk and
  virtio-net (NET-A). Bound by **ADR-021** (W^X). Unblocks DDR-701's deferred
  visual toggle and the Layer-7 compositor.

> **Why an ADR?** The Layer-7 brief is explicitly blocked on a GPU framebuffer +
> modeset. This slice adds the first display output path. The device model, the
> DMA/addressing model, and the bring-up verification must be fixed before code.

---

## Decisions

### D1 — Device: virtio-gpu-pci (modern virtio 1.0, 2D)
Target QEMU's `virtio-gpu-pci` (vendor `0x1AF4`, PCI base class `0x03` Display).
Use only the **2D** command set on the **control queue** (queue 0); the cursor
queue (1) and 3D/virgl are out of scope. Detected in kmain's existing PCIe scan
(`class_code == 0x03`), mirroring the virtio-blk/net dispatch.

### D2 — Single-buffer linear framebuffer, BGRA8888
One host resource (`RESOURCE_CREATE_2D`, format `B8G8R8A8_UNORM`) sized to scanout
0's mode (from `GET_DISPLAY_INFO`; fallback 1024×768). Its backing is a physically
contiguous linear buffer from the **PMM pool** (`pmm_alloc_pages`) — the low 1 GiB
is identity-mapped, so the returned address is both the physical address handed to
the device (`RESOURCE_ATTACH_BACKING`) and the virtual address the kernel draws
into. Double-buffering / page-flipping is deferred to the compositor slices; this
slice proves a single front buffer end to end.

### D3 — DMA + completion: poll the used ring at boot
Control-queue requests are a 2-descriptor chain `[request (device-read),
response (device-write)]`, both in a PMM scratch page. Bring-up runs once in
kmain (single-threaded, scheduler not yet relevant), so completion is by
**polling** `virtq_pop_used` after `virtio_pci_notify` (bounded spin) rather than
the IRQ/`sched_block` path virtio-blk uses at runtime — deterministic and
independent of interrupt state. Every command checks the response type
(`RESP_OK_NODATA` / `RESP_OK_DISPLAY_INFO`); a non-OK or a poll timeout aborts the
bring-up cleanly (log + return), never a panic.

### D4 — Bring-up sequence (and the gate)
`GET_DISPLAY_INFO` → `RESOURCE_CREATE_2D` → `RESOURCE_ATTACH_BACKING` →
`SET_SCANOUT` → draw a deterministic test pattern into the FB →
`TRANSFER_TO_HOST_2D` → `RESOURCE_FLUSH`. On success print
`PRADYOS_GPU_FB_OK <w>x<h>`. Gate **`smoke-gpu`** boots with an attached
`virtio-gpu-pci` and greps the sentinel — headless QEMU still processes and ACKs
the 2D commands (the FB simply isn't shown), so the full control path is
verifiable without a display, exactly like the NET-A virtio-net bring-up gate.

### D5 — Isolation
Only the `smoke-gpu` gate attaches a GPU device; other gates boot without one and
`gpu_init` is never dispatched (no class-0x03 device). The driver shares INTx
with the other virtio devices (the IDT chains handlers already), but bring-up
polls, so it does not depend on GPU interrupts.

## Security / robustness
- All device-shared memory is PMM-pool, physically contiguous, identity-mapped;
  no higher-half kernel pointer is ever handed to the device.
- Bounded: one resource, one scanout, fixed max FB (≤ 4 MiB / order-10); a failed
  or absent device degrades to no-op, never a panic.
- No user/ring-3 surface in this slice (kernel-only bring-up). A `/dev/fb0`-style
  ring-3 mapping + the compositor come in later slices.

## Alternatives considered
- **Legacy VGA / VBE linear FB** — rejected: QEMU's std-VGA path is a dead end for
  the wlroots/VirtIO-GPU compositor the brief targets; building on virtio-gpu now
  matches §13 ("60fps on VirtIO-GPU QEMU").
- **IRQ-driven bring-up** — rejected for slice 0: polling is simpler and removes a
  dependency on interrupt timing during early boot; the runtime compositor can
  move to IRQ/fence completion later.
- **Double buffering now** — deferred: not needed to prove the output path; adds
  flip/fence complexity better designed with the compositor.
