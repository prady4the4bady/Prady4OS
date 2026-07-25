# DDR-774 — NVMe completion IRQ: blast-radius review and three-way split

**Status:** **scoped / deferred — no code this slice.** This DDR exists to record a
blast-radius finding that changes the roadmap, and to define the bounded slices
that must replace the single "NVMe IRQ" item.
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 1**.

## Why this was reviewed now

DDR-771 relocated the virtio-blk MSI-X window to 56–63, vacating vectors **50–53**,
and `idt.c` already gates 0–63 with `MSIX_VEC_COUNT = 14` covering 50–63. That made
"NVMe IRQ" look like a small slice: pick a free vector, `msix_register` a handler,
set `IEN` on the I/O completion queue. The mandated pre-code blast-radius review
shows that framing is wrong.

## Finding — the slice spans three coupled surfaces

1. **No generic PCI MSI-X programmer exists.** The only implementation is
   `virtio_pci_msix_setup()` in `kernel/drivers/virtio/virtio_pci.c`, which is
   virtio-coupled by construction: it takes `struct virtio_pci_dev *`, uses that
   struct's `cfg8`/`cfg32` config accessors and virtio's own `map_bar()` VA window,
   and finishes by programming `virtio_pci_common_cfg.queue_msix_vector` for each
   queue. NVMe has no `virtio_pci_dev` and no common-cfg. Reusing it therefore means
   **refactoring a helper out of the virtio path that currently serves blk, net,
   gpu and input** — the same shared surface whose last change (DDR-771) produced a
   `#GP` (ungated IDT vector 56) *and* a CI break from a stale gate sentinel
   (`msix vec=50`). Duplicating it instead is barred by the no-patchwork rule.
2. **The NVMe BAR mapping is too small and possibly the wrong BAR.** `nvme.c` maps
   a fixed `NVME_BAR_MAP = 0x2000` (2 pages) of BAR0 at `NVME_BAR_VBASE`. The MSI-X
   table lives at the offset named by the capability's table-offset field, in the
   BAR named by its 3-bit BIR — both must be read at runtime, and neither is
   guaranteed to fall inside the current 2-page BAR0 window. So the mapping path
   changes as well.
3. **Completion changes concurrency model.** Today `nvme_submit()` polls the CQ
   phase bit in *thread* context and owns the CQ head doorbell exclusively. IRQ-
   driven completion moves that work into *interrupt* context, so CQ head/phase
   ownership, the wake handshake, and re-entrancy all have to be reasoned about
   for **every** NVMe I/O, including the DDR-772 multi-command PRP loop.

Three coupled surfaces — one of them shared by four drivers — is not a bounded
slice, and the selection policy prefers "bounded, gateable, least likely to create
architecture debt". Per the standing rule, invasive work is not started on this
basis; it is split first.

## Decision — split into three bounded slices (execute in order)

- **DDR-774a — generic PCI MSI-X helper (pure refactor, no behaviour change).**
  Extract capability-walk + table programming into a `pcie_*` helper taking
  (bus, dev, func, table VA, vector, apic_id); reimplement
  `virtio_pci_msix_setup()` on top of it, leaving the queue-vector programming in
  the virtio layer. **Gate:** existing `smoke-fs` (asserts `msix vec=56`),
  `smoke-net`, `smoke-input`, `smoke-gpu` must stay green — a behaviour-preserving
  refactor is fully covered by them. Grep `Makefile`/`tools`/`.github` for
  vector-string sentinels **before** editing (DDR-771 lesson).
- **DDR-774b — NVMe MSI-X table mapping + `IEN`.** Read the capability's
  table offset/BIR at runtime, map that BAR region (extend/replace the fixed
  `NVME_BAR_MAP`), program entry 0 via the 774a helper, set `IEN` (Create I/O CQ
  `cdw11` bit 1) with the vector in bits 31:16. **Still polls for completion** —
  so this lands with `smoke-nvme` unchanged and green, proving the IRQ plumbing is
  inert-but-correct before behaviour changes.
- **DDR-774c — IRQ-driven completion with bounded spin fallback.** Convert
  `nvme_submit()` to wait on an IRQ-set completion flag with a **bounded** spin
  fallback so a lost or misrouted interrupt can never hang the boot (**S2**).
  **Gate:** extend `smoke-nvme` with a count-based `PRADYOS_NVME_IRQ_OK <n>`
  sentinel (deterministic, never wall-clock) while `PRADYOS_NVME_RW_OK` and
  `PRADYOS_NVME_PRP_OK` stay green.

## Architecture prerequisite checklist (for the 774a/b/c family)

- New NSI/syscalls: **none** (NSI stays at 75).
- TCB / roster / agent-slot fields: **none**.
- PMM/VMM shared mappings: **yes, 774b** — a new/extended MMIO mapping for the
  MSI-X table (uncached, `VMM_RW | VMM_PCD`), not a `PTE_SW_SHARED` case.
- Capability gates: **none** — kernel-internal driver path, no ring-3 authority.
- AETHER queue/audit record types: **none**.
- Scheduler/accounting hooks: **774c only** if the wait ever blocks rather than
  spins; the bounded-spin design deliberately avoids adding a scheduler hook.
- Filesystem/root-mount constraints: **none** (below the block layer).
- Network policy tables: **none**.
- Compositor/UI exposure: **none**.
- New smoke gate: none new — 774a/b reuse existing gates; 774c extends
  `smoke-nvme` with a deterministic counter sentinel.
- **Security invariants:** **S2 (bounded everything)** — the 774c fallback must be
  bounded so a missing IRQ degrades to polling instead of hanging. **S6 (fault
  isolation)** — an interrupt-context bug must not be able to panic the kernel or
  corrupt an unrelated driver's state; 774a must not change virtio behaviour.
  S1/S3–S5/S7/S8 are not engaged (no agent, capability, audit or objective-function
  surface). Nothing here touches W^X, NX, or any capability contract.

## Consequence for the master doc

`Section B item 1` is re-scoped from "High / needs a free vector" to the 774a→b→c
chain, with the corrected note that **vector availability was never the real
blocker** — the virtio coupling, the BAR mapping, and the completion concurrency
model are.
