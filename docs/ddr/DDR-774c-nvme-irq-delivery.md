# DDR-774c — NVMe MSI-X delivery: root cause and fix (phase c-1)

**Status:** **c-1 implemented — root cause found and fixed.** A boot now prints
`[nvme] irqs=6` (was `irqs=0`) and `PRADYOS_NVME_IRQ_OK`. **c-2 (converting the
completion path to IRQ-driven waiting) is deliberately NOT in this slice** — see
"Remaining work".
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 1**
(NVMe IRQ), sub-slice **774c**. Parents: `DDR-774-nvme-irq-scoping.md`,
`DDR-774a-generic-pcie-msix.md`, `DDR-774b-nvme-msix-table.md`.

## Root cause — a spec misreading in DDR-774b (mine)

774b created the I/O completion queue with:

```c
cq_cdw11 |= (1u << 1) | ((uint32_t)NVME_MSIX_VEC << 16);   /* IEN | vector */
```

passing the **x86 interrupt vector (50)** in CDW11[31:16]. That field is the NVMe
**Interrupt Vector (IV)**, which is an **index into the device's MSI-X table** —
*not* an architectural interrupt vector. The controller was therefore told to
signal using **MSI-X table entry 50**, while 774b had programmed only **entry 0**.
Entry 50 is unprogrammed and masked, so the controller never raised anything:
exactly the observed `irqs=0`.

The x86 vector is carried in the *message-data* field of the table entry, which
`pcie_msix_program()` already writes. So the fix is to pass the table **index**:

```c
#define NVME_MSIX_ENTRY 0u                  /* table entry we programmed */
cq_cdw11 |= (1u << 1) | ((uint32_t)NVME_MSIX_ENTRY << 16);   /* IEN | IV */
```

**Evidence:** identical build/boot before and after the one-line change —
`[nvme] irqs=0` → **`[nvme] irqs=6`**, with `PRADYOS_NVME_RW_OK` and
`PRADYOS_NVME_PRP_OK` unchanged.

### Hypotheses that were NOT the cause (checked, not assumed)

- **Interrupts masked** — ruled out in 774b: `sti` at `kernel/main.c:1478` and
  `:1737` both precede `nvme_init()` at `:1801`.
- **MSI-X Function Mask (message-control bit 14) never explicitly cleared** — this
  was the leading suspect going in, and it is **not** the cause. It is 0 after
  reset, and delivery now works without touching it. `pcie_msix_program()` is
  therefore left **unchanged**, which also avoids editing the shared 774a helper
  that serves blk/net/gpu/input. (Clearing it explicitly remains a reasonable
  future hardening, but it would be a change with no observed defect behind it.)
- **Table address math / BIR decode in `nvme_msix_setup()`** — also not the cause;
  entry 0 was programmed correctly all along, as proven by delivery now working
  with the *same* mapping code.

That the cheapest suspect was wrong is the reason c-1 was required to produce a
**non-zero count** as its success criterion rather than "apply the likely fix".

## Gate

`smoke-nvme` gains **`PRADYOS_NVME_IRQ_OK`** (asserted) and forbids
`PRADYOS_NVME_IRQ_FAIL`, alongside the existing `[nvme] msix vec=50`,
`PRADYOS_NVME_RW_OK`, `PRADYOS_NVME_PRP_OK`.

**Determinism:** completion is still polled, so the *final* command's interrupt
could in principle still be in flight at print time. Rather than assert a
timing-dependent count, the self-test performs a **bounded** settle spin
(≤1e6 `pause`, exits immediately in practice because earlier commands' interrupts
were raised long before) and then asserts `irqs > 0`. The exact count (6) is
printed for observability but **not** asserted, since it would be brittle against
any future change in how many commands the self-test issues. Gate count stays
**106**.

## Blast radius

`kernel/drivers/nvme/nvme.c` only (a constant, the CDW11 field, and the self-test
assertion), plus the `smoke-nvme` sentinel line and docs. **No change to the
shared `pcie_msix_program()`**, so the four virtio MSI-X consumers are untouched
and cannot regress.

## Architecture prerequisite checklist

Inherited from DDR-774. For c-1 specifically: no NSI/syscalls, no TCB/roster
fields, no new PMM/VMM mapping (774b's window is reused unchanged), no capability
gate, no AETHER queue/audit record, no scheduler hook, no filesystem/root-mount
dependency, no network policy table, no compositor/UI exposure, no new gate.

**Security invariants:** **S2 (bounded everything)** — the new settle loop is
explicitly bounded, and no unbounded wait is introduced anywhere. **S6 (fault
isolation)** — the ISR still only increments a counter and never touches CQ
head/phase, so it cannot race the polling loop that owns them; MSI-X is per-device
and unshared, so NVMe cannot disturb virtio's vectors. S1/S3–S5/S7/S8 are not
engaged. Nothing here touches W^X, NX, or any capability contract.

## Remaining work — c-2 (next slice)

Delivery is proven, so the completion path can now be converted: make
`nvme_submit()` wait on an IRQ-set completion flag with a **bounded** spin
fallback (S2) so a lost or misrouted interrupt degrades to polling instead of
hanging the boot. The ISR must stay bounded and must take ownership of CQ
head/phase *from* the polling loop atomically rather than concurrently with it
(S6) — that ownership transfer, not the interrupt itself, is the real risk in c-2,
which is why it is a separate slice.
