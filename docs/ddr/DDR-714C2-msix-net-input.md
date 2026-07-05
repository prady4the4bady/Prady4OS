# DDR-714 stage C2 — MSI-X for virtio-net + virtio-input

> DDR before code. Extends C1's per-device MSI-X to the remaining INTx
> consumers. After this NO virtio device uses the 8259; the only lines left on
> the PIC are ISA (keyboard IRQ1, COM1 RX IRQ4) — the I/O APIC slice (C3) or a
> later decision point.

## Decisions
- **D1 — multi-queue routing.** `virtio_pci_msix_setup` gains `nqueues`: it
  routes queues `0..nqueues-1` ALL to table entry 0 (one vector per device —
  per-queue vectors are an optimization, not needed at our rates). virtio-net
  has RX(0)+TX(1); virtio-input event(0)+status(1); blk passes 1. Each queue's
  `queue_msix_vector` write is read-back-verified as in C1.
- **D2 — vectors 54 (net) and 55 (input).** IDT stubs grow 54→56;
  `MSIX_VEC_COUNT` 4→6. Handlers are the drivers' existing bodies minus the
  INTx `isr_ack` read (the C1 pattern); INTx fallback preserved.
- **D3 — destination stays the BSP.** net_irq wakes lwIP consumers and
  input_irq pushes to rings consumed under `cli` locals — the cross-CPU
  completion-field review still gates distribution (C3).

## Gate
None new: `smoke-net`/`-lo`/`-fuzz` (net) and `smoke-mouse`/`smoke-drag`/
`smoke-evresize` (virtio-input tablet) ride the vectors every run.
`smoke-net-lo` asserts `msix vec=54` (no silent fallback, mirroring C1's
smoke-fs pin); the tablet's vec-55 engagement prints on the GPU gates and its
functional proof is the pointer gates themselves (QMP moves/clicks arrive only
via the vector). 61 gates.

## Non-goals
Per-queue vectors; distributing vectors to APs (C3 + completion review); the
ISA lines / I/O APIC; the GPU (polls — no IRQ path at all).
