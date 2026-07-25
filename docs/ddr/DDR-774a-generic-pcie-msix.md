# DDR-774a — generic PCI MSI-X capability programmer (pure refactor)

**Status:** implemented — all five covering gates PASS: `smoke-fs` (12 patterns,
incl. **`msix vec=56`** — virtio-blk), `smoke-net-lo` (3 patterns, incl.
**`msix vec=54`** — virtio-net), `smoke-net` (TCP echo), `smoke-input`
(`PRADYOS_INPUT_OK`), `smoke-gpu`. Image builds `-Werror` clean. All three virtio
callers unchanged. First bounded slice of the DDR-774 split.
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 1**
(NVMe IRQ), sub-slice **774a**. Parent: `docs/ddr/DDR-774-nvme-irq-scoping.md`.

## Problem

The only MSI-X programmer in the tree is `virtio_pci_msix_setup()` in
`kernel/drivers/virtio/virtio_pci.c`. It mixes two concerns:

1. **Generic PCI**: walk the capability chain for ID 0x11, read the table
   offset/BIR, set MSI-X-enable in message control, program table entry 0
   (message address `0xFEE00000 | apic_id<<12`, data = vector, vector-control
   unmasked), disable INTx.
2. **virtio-specific**: map the BAR via virtio's own `map_bar()`, and point every
   queue at table entry 0 through `virtio_pci_common_cfg.queue_msix_vector`.

Part 1 is device-agnostic but reachable only through a `struct virtio_pci_dev *`,
so NVMe (DDR-774b) cannot use it. Duplicating it would violate the no-patchwork
rule.

## Decision — extract part 1 into `kernel/drivers/pcie/pcie.c`

Three small helpers, declared in `pcie.h`:

- `uint8_t pcie_msix_find(bus, dev, func, uint8_t *bir, uint32_t *table_off)` —
  walk the capability chain from config `0x34` for ID 0x11; return the capability's
  config offset (0 = absent) and fill BIR (bits 2:0 of the table dword) and byte
  offset (bits 31:3).
- `void pcie_msix_program(bus, dev, func, cap, volatile uint32_t *entry0, vector,
  apic_id)` — set MSI-X-enable, then program entry 0's four dwords.
- `void pcie_intx_disable(bus, dev, func)` — set command-register bit 10.

`virtio_pci_msix_setup()` keeps its **exact signature and call order** and is
reimplemented on top of them; the per-queue `queue_msix_vector` loop stays in the
virtio layer, where it belongs.

**Ordering is preserved deliberately.** Today INTx-disable runs *after* the
per-queue vector programming, so it is kept at that call site rather than folded
into `pcie_msix_program`. Folding it in would have moved it earlier and made this
something other than a pure refactor. `pcie_intx_disable()` exists so DDR-774b can
reuse it at whatever point NVMe needs.

Capability offsets are DWORD-aligned by the PCI spec (the existing code already
relies on this via `& 0xFC`), so the byte fields are read out of `pcie_read32`
without adding a byte accessor.

## Behaviour contract

**Zero behaviour change.** Same register writes, same values, same order, same
return codes, and **no print strings altered** (verified: the only MSI-X strings a
gate asserts are `msix vec=54` and `msix vec=56`, neither of which this touches).

## Blast radius

`kernel/drivers/pcie/pcie.{c,h}` (additive) and `kernel/drivers/virtio/virtio_pci.c`
(internal rewrite). The three callers — `virtio_blk.c:240`, `virtio_input.c:129`,
`virtio_net.c:181` — are **unchanged**, because the signature is preserved. If the
refactor had forced changes in those drivers, the slice would have been stopped
and re-reported instead of expanded.

## Gates

A behaviour-preserving refactor of the shared MSI-X path is fully covered by
existing gates exercising all four MSI-X consumers. Note the two vector-string
assertions live in **different** gates — checked rather than assumed:
`msix vec=56` (virtio-blk) is asserted by **`smoke-fs`**, and `msix vec=54`
(virtio-net) by **`smoke-net-lo`** (not `smoke-net`, which is the TCP-echo gate).
Full set: `smoke-fs`, `smoke-net-lo`, `smoke-net`, `smoke-input`, `smoke-gpu`.
No new gate is warranted for a no-op refactor. Gate count stays **106**.

## Architecture prerequisite checklist

Inherited from DDR-774 (answered there for the 774a/b/c family). For 774a
specifically: no NSI/syscalls, no TCB/roster fields, no new PMM/VMM mapping (the
caller still supplies the mapped table VA), no capability gate, no AETHER
queue/audit record, no scheduler hook, no filesystem/root-mount dependency, no
network policy table, no compositor/UI exposure, no new gate.

**Security invariants:** **S6 (fault isolation)** is the governing one — the
refactor must not change what any of the four virtio drivers program, since a
mistake in the shared path could corrupt an unrelated device's interrupt routing.
It is discharged by preserving register writes/order exactly and by requiring all
four existing MSI-X gates to stay green. **S2** is untouched (no new bounds).
S1/S3–S5/S7/S8 are not engaged: no agent, capability, audit, or objective-function
surface. Nothing here touches W^X, NX, or any capability contract.

## Non-goals

- NVMe MSI-X table mapping and `IEN` (that is **774b**).
- Any change to completion polling (that is **774c**).
- Multi-vector / per-queue distinct vectors (still one vector per device).
