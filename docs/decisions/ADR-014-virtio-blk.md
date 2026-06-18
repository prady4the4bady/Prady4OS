# ADR-014: virtio transport + virtio-blk + generic block layer

- **Status:** Accepted 2026-06-18
- **Phase:** 3 (slice 3b)

## Context

The first real device driver on the PCIe foundation. Built as a **reusable
virtio transport** so virtio-net (and any future virtio device) share it rather
than each carrying a copy.

## Decision

- **Transport, modern virtio 1.0** (`kernel/drivers/virtio/`):
  - `virtio_ring.{c,h}` — split-virtqueue structures (desc table, avail, used)
    and the memory barriers (compiler fence for ring publish on x86 TSO; MFENCE
    before the MMIO notify).
  - `virtio.{c,h}` — the `virtq` object: alloc rings from the PMM (page-aligned,
    identity-mapped so phys == ptr), a free-descriptor list, add a descriptor
    chain, publish to avail, reap the used ring, free chains.
  - `virtio_pci.{c,h}` — PCI transport: enable memory-space + bus-master (DMA)
    in the command register; walk the vendor PCI capabilities to find the
    common/notify/isr/device config structures; map their BAR(s) uncached
    (`VMM_PCD`); the device-status machine (RESET→ACK→DRIVER→FEATURES_OK→
    DRIVER_OK), 64-bit feature negotiation (requires VIRTIO_F_VERSION_1), queue
    programming (write desc/avail/used physical addresses), and notify.
- **Block layer** (`kernel/drivers/blk/blk.{c,h}`): generic `blk_device`
  registry + `blk_read`/`blk_write` dispatch. NVMe/ATA will register here too.
- **virtio-blk** (`kernel/drivers/blk/virtio_blk.c`): a 3-descriptor request
  (header RO | data | status WO); **interrupt-driven completion** — the INTx
  handler (registered via `irq_register`, line unmasked via `pic_unmask`) reads
  the ISR (deasserts the level-triggered line), reaps the used ring, and wakes
  the blocked caller. No busy polling. One request in flight (single queue).

## Consequences / deferred

- INTx (legacy) interrupts via the 8259 PIC; MSI/MSI-X await the APIC.
- One in-flight request; a request-tag table enables concurrency later.
- I/O buffers must be identity-mapped (phys == virt); arbitrary-buffer support
  needs a virt→phys helper.
- Kernel image now loaded in 8×64-sector chunks (256 KiB cap) by Stage 2 — a
  larger/relocating loader is needed when the kernel exceeds that.

## Verification

QEMU q35 (boot from virtio-blk): negotiation completes; `blk_read(sector 0)`
returns the MBR (boot signature 0xAA55); a write→read round-trip on sector 100
matches; completion is interrupt-driven (INTx IRQ 11). smoke PASS; warning-free
`-Werror` build.
