# DDR-774b — NVMe MSI-X table mapping + `IEN` (plumbing only, still polling)

**Status:** implemented **with a documented open issue** — `smoke-nvme` PASS
(6 patterns: `[nvme] msix vec=50`, `PRADYOS_NVME_RW_OK`, `PRADYOS_NVME_PRP_OK`,
…), image `-Werror` clean, polled path unaffected. **But the interrupt is not
being delivered: the boot prints `[nvme] irqs=0`.** See "Finding" below — this
slice proves the table is *found, mapped and programmed*, **not** that an
interrupt *arrives*. Root-causing delivery is now **774c's first task**, before
any completion-path change. Second bounded slice of the DDR-774 split.
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 1**
(NVMe IRQ), sub-slice **774b**. Parent: `docs/ddr/DDR-774-nvme-irq-scoping.md`;
depends on **774a** (`pcie_msix_find` / `pcie_msix_program` / `pcie_intx_disable`,
already on `main` @ `347c422`).

## Problem

NVMe completions are polled. Before the completion path can become interrupt-
driven (774c), the controller must actually be able to *raise* an interrupt:
its MSI-X table has to be located, mapped and programmed, the I/O completion
queue has to be created with interrupts enabled, and a handler has to be routed.

Doing that **and** switching the completion path in one slice would change
plumbing and behaviour simultaneously, so a failure could not be attributed to
either. 774b therefore lands the plumbing while **completion stays polled** — the
interrupt is armed but nothing depends on it yet, so `smoke-nvme` must stay green
with its existing sentinels. That makes 774c a pure, isolated behaviour change.

## Decision — `kernel/drivers/nvme/nvme.c`

1. **Locate the table** with `pcie_msix_find(bus, dev, func, &bir, &table_off)`.
   Both the BAR index and the byte offset are runtime values — they are *not*
   assumed to be BAR0/inside the existing window.
2. **Map it.** `nvme.c` today maps a fixed 2-page (`NVME_BAR_MAP = 0x2000`) window
   of BAR0 at `NVME_BAR_VBASE`, which need not contain the table. Add a small
   `bar_base(bus,dev,func,idx)` helper (same 64-bit-BAR decode already used for
   BAR0, at config `0x10 + idx*4`) and map **2 pages** covering the table's page
   at a dedicated `NVME_MSIX_VBASE` window, uncached (`VMM_RW | VMM_PCD`).
   Two pages because the MSI-X table offset is only 8-byte aligned by spec, so a
   16-byte entry could straddle a page boundary. Entry 0 is then
   `NVME_MSIX_VBASE + (table_off & 0xFFF)`.
   A separate window (rather than growing the BAR0 map) keeps the existing
   register mapping untouched — smaller blast radius.
3. **Program entry 0** via `pcie_msix_program(..., cap, entry0, NVME_MSIX_VEC,
   lapic_id())` and register the handler with `msix_register(NVME_MSIX_VEC, fn)`.
   **Vector 50** — DDR-771 vacated 50–53; `idt.c` gates 0–63 and
   `MSIX_VEC_COUNT = 14` covers 50–63, so no IDT/stub change is needed.
4. **Enable interrupts on the I/O CQ.** Create I/O Completion Queue `CDW11` gains
   `IEN` (bit 1) and the vector in bits 31:16, alongside the existing `PC` (bit 0).
5. **Handler is inert:** it increments a counter and returns (the MSI-X dispatch in
   `idt.c` already does the LAPIC EOI). It must not touch the CQ head/phase — that
   is 774c's job, and touching it here would race the polling loop.
6. `pcie_intx_disable()` once MSI-X is live.

`nvme_submit()` is **unchanged** — completion is still polled.

## Gate

`smoke-nvme`, extended with **`[nvme] msix vec=50`** — printed unconditionally
once the table is found, mapped and programmed, so it is fully deterministic
(no timing dependence). `PRADYOS_NVME_RW_OK` and `PRADYOS_NVME_PRP_OK` must stay
green, proving the polled path is unaffected by arming the interrupt.

The **IRQ *count*** is deliberately **not** asserted here: with polling still
active, delivery timing relative to the print is not guaranteed, and a
timing-dependent sentinel would violate the deterministic-gates rule (DDR-735/771
lessons). The count is printed for observability and becomes a gated assertion in
**774c**, where the completion path genuinely depends on it. Gate count stays
**106**.

## Finding — MSI-X delivery is NOT yet working (`irqs=0`)

With the table programmed and the I/O CQ created with `IEN` + vector 50, a full
boot still reports **`[nvme] irqs=0`** after both the 4 KiB and 16 KiB I/O
round-trips have completed. Two candidate explanations were checked and one is
**eliminated**:

- **Not an interrupts-masked artifact.** `sti` executes at `kernel/main.c:1478`
  and `:1737`, both well before `nvme_init()` at `:1801`, so `IF` is set while the
  NVMe self-test runs. The count is therefore not merely being read "too early
  with interrupts off".

Remaining candidates for 774c to root-cause, in rough order of likelihood:
1. **Message-control bits.** `pcie_msix_program()` sets MSI-X Enable (MC bit 15)
   but never explicitly clears the **Function Mask (MC bit 14)**. It is 0 after
   reset, so virtio works, but NVMe's state at this point is not verified.
2. **Table address math** in `nvme_msix_setup()` — the BAR-base/offset split
   (`page = (bbase+table_off) & ~0xFFF`, entry at the in-page remainder). No #PF
   occurred, so the write landed on mapped memory, but not necessarily on the
   controller's actual table.
3. **Per-entry Vector Control** — entry dword 3 is written 0 (unmasked) by
   `pcie_msix_program`, which should be correct; worth re-reading back.

**This is why the IRQ count was deliberately left ungated.** Had 774c been
attempted in one step, the completion path would have silently fallen back to its
bounded spin on every I/O and the regression would have been invisible. Landing
the plumbing separately turned an unknown into a measured, isolated symptom.

## Blast radius

`kernel/drivers/nvme/nvme.c` only, plus the `smoke-nvme` sentinel line and docs.
No change to `pcie.{c,h}` (774a already provides the helpers), no change to
`idt.c` (vector 50 is already gated and dispatchable), and no change to any other
driver. If mapping the table turned out to require altering the shared BAR0
window or the IDT, the slice would be stopped and re-reported rather than widened.

## Architecture prerequisite checklist

- New NSI/syscalls: **none** (NSI stays at 75).
- TCB / roster / agent-slot fields: **none**.
- PMM/VMM shared mappings: **yes** — one new 2-page uncached MMIO window
  (`NVME_MSIX_VBASE`) for the MSI-X table. Not a `PTE_SW_SHARED` case; kernel-only.
- Capability gates: **none** — kernel-internal driver path, no ring-3 authority.
- AETHER queue/audit record types: **none**.
- Scheduler/accounting hooks: **none** (the inert handler does not wake anything;
  that arrives in 774c and is deliberately designed to spin, not block).
- Filesystem/root-mount constraints: **none** (below the block layer).
- Network policy tables: **none**.
- Compositor/UI exposure: **none**.
- New smoke gate: none — `smoke-nvme` extended with one deterministic sentinel.
- **Security invariants:** **S2 (bounded everything)** — the table mapping is a
  fixed 2 pages and the handler does bounded work (one increment); no unbounded
  loop or allocation is introduced, and the polled completion keeps its existing
  bounded spin. **S6 (fault isolation)** — the handler must not touch CQ state
  shared with the polling loop, so a spurious or early interrupt cannot corrupt an
  in-flight command or panic the kernel; MSI-X is per-device and unshared, so it
  cannot disturb virtio's vectors. S1/S3–S5/S7/S8 are not engaged (no agent,
  capability, audit, or objective-function surface). Nothing here touches W^X, NX,
  or any capability contract.

## Non-goals

- IRQ-driven completion / removing the poll (that is **774c**).
- Asserting the IRQ counter in a gate (774c, once it is deterministic).
- Multiple vectors or per-queue vectors (one vector, entry 0, as with virtio).
