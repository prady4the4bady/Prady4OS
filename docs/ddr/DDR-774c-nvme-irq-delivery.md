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

## c-2 — STOPPED and re-scoped (stop condition invoked, no code written)

c-2 was to convert `nvme_submit()` to "wait on an IRQ-set flag with a bounded spin
fallback, without a scheduler hook". On inspection that specification cannot
deliver what it promises, so per the standing stop condition ("if c-2 needs
locking/scheduler changes beyond `nvme.c`, STOP and re-scope rather than expand
silently") **no code was written**. Reasons:

1. **The completion path is already a bounded poll.** `nvme_submit()` spins on the
   CQE phase bit for ≤5e7 iterations and then returns `0xFFFF`. Adding "re-check
   when the IRQ counter moves" changes it into *polling with an interrupt hint* —
   the phase bit still drives correctness, and no CPU time is actually saved.
   That is churn on the hottest path in the driver for no measurable benefit.
2. **A genuine IRQ-driven wait means the CPU sleeps**, which requires one of:
   - `sti; hlt` — **rejected**: it would enable interrupts behind the caller's
     back, changing `IF` state the caller may be relying on (a correctness hazard,
     not a style question);
   - plain `hlt` guarded by a runtime `IF` check — stays inside `nvme.c` and is
     bounded (the 100 Hz timer guarantees a wake), but introduces a **new idle
     path into every NVMe I/O**; or
   - a scheduler **block/wake wait-queue** — the only design that is actually
     correct and efficient, and explicitly **out of scope** for this slice.
3. **Sequencing.** Option 2b is the plausible bounded middle ground, but adding a
   new idle/halt path while an **unexplained `-smp 4` boot hang is open**
   (Section B#3 — run 30151522978 hung after `SYSFSTAT OK` at the full 180 s) is
   poor sequencing: it would muddy attribution if the hang recurs. B#3 should be
   root-caused first.

**Consequence for Section B#1:** 774a, 774b and 774c-1 ship — the NVMe MSI-X path
is programmed, delivered and gated (`irqs=6`, `PRADYOS_NVME_IRQ_OK`). The
remaining "use the interrupt to sleep instead of poll" work is deliberately
deferred and re-scoped as a **future DDR** that must decide, up front, between the
`hlt`-idle variant and a real scheduler wait-queue — with a measured justification
(current polling cost) rather than an assumption that IRQ-driven is better.
B#1 is therefore **functionally complete for correctness**, with a documented,
optional performance follow-on.
