# DDR-1050 — I/O APIC stage D (disable the 8259) ASSESSED, safe subset identified, FULL migration BLOCKED

**Status:** ASSESSED and NOT BUILT, with the blocker named — the shape DDR-1038
used for `SYS_FUTEX`.
**Group A row:** *"I/O APIC migration — DDR-714 stage D — disable 8259, route ISA
IRQs through I/O APIC — gate `smoke-ioapic`."*

---

## 1. What already exists (measured, not assumed)

The row reads as though nothing is built. Most of it is. `kernel/apic/ioapic.c`
(198 lines, DDR-874) ships MADT parsing, MMIO mapping, `ioapic_route()`,
`ioapic_mask/unmask()`, and — the part that is easy to get wrong — an **Interrupt
Source Override** table, so no path uses a raw IRQ number as a GSI. It is called
from `main.c:3646` and its own comment states the deliberate limit: *"the 8259
PIC stays in charge — this does not disturb the existing routing, it adds the
table per-CPU affinity will be expressed in."*

Measured on a real boot (`panicscan_3.log`):

```
[ioapic] gsi_base=0 redirs=24 overrides=5
```

So the hardware is present, 24 redirection entries, 5 overrides parsed. **Stage D
is the cutover only.**

## 2. What is actually still on the 8259 — and the count is not one

The obvious reading is "only the keyboard is left". That is **wrong**, and
`grep` says so. Four call sites claim ISA lines:

| site | line | nature |
|---|---|---|
| `console.c:236-237` | **IRQ4, COM1 RX** | genuine ISA, **unconditional** |
| `idt.c:694` | **IRQ1, PS/2 keyboard** | genuine ISA |
| `virtio_net.c:185-186` | `g_dev.irq` | **PCI INTx fallback** when MSI-X is unavailable |
| `virtio_input.c:221-222` | `g_dev.irq` | PCI INTx fallback |
| `virtio_blk.c:416-417` | `v->dev.irq` | PCI INTx fallback |

PIT IRQ0 is already masked by DDR-714 stage A (`lapic.c:191`), so it is not in
scope.

**IRQ4 is the one that matters operationally**: COM1 RX is how every gate feeds
input to PRISM. Breaking it breaks `smoke-shell` and everything downstream.

## 3. THE BLOCKER — there is no ACPI `_PRT` parser

`grep -rn '_PRT\|PCI_ROUTE\|prt_' kernel/` returns **nothing**.

The three virtio fallbacks register **PCI** interrupt lines, not ISA ones. A
MADT Interrupt Source Override table describes *ISA* overrides; it does not say
which GSI a PCI device's INTA# lands on. That mapping lives in the ACPI
namespace's `_PRT` objects, and this kernel cannot read them.

So a complete stage D — 8259 masked, every line on the I/O APIC — **cannot be
done correctly today**. Routing those lines would mean guessing a GSI, and a
wrong guess arms an input nothing is wired to: no interrupts, looking exactly
like a dead device. That is the failure mode `ioapic.c`'s own header warns about
for the ISA case.

These fallbacks are **dormant in this environment** — measured, every virtio
device takes MSI-X:

```
virtio-blk: blk0..blk3 ready, msix vec=56..59
[net] virtio-net up ... msix vec=54
[blk] msix on AP OK
```

**Dormant is not absent.** The fallback exists precisely for a machine without
usable MSI-X, and silently breaking it because QEMU never takes that path would
be an unmeasured assumption about hardware this project cannot test — the
DDR-1045 failure mode, in a place where the consequence is a dead device rather
than a red CI job.

## 4. AND THE BENEFIT IS ALREADY DELIVERED

This is the part that decides it. `ioapic.c`'s header gives the reason the I/O
APIC exists here:

> *"That is the mechanism per-CPU interrupt affinity needs; without it 'route
> this device to that core' has nowhere to be expressed."*

**Every device that carries real traffic already has per-CPU affinity, via
MSI-X** — DDR-714C1/C3 and DDR-771 route virtio-blk over vectors 56–63
round-robin across CPUs, virtio-net at 54, virtio-input at 55. The measured boot
above confirms it.

What stage D would additionally move is the **PS/2 keyboard and COM1 serial**:
two low-rate lines whose affinity nobody needs, on the input path every gate
depends on. The migration would touch the highest-risk path in the tree to buy
an affinity benefit that MSI-X has already provided everywhere it matters.

## 5. The safe subset, recorded so it is not re-derived

If this is ever taken up, the buildable part is IRQ1 + IRQ4 only, and the
ordering is the non-obvious bit:

```c
pic_mask_all();              /* 8259 out first — no double-delivery window */
g_isa_via_ioapic = 1;        /* EOI target flips BEFORE the line is armed  */
ioapic_route(1, 32 + 1, bsp_apic_id);
ioapic_route(4, 32 + 4, bsp_apic_id);
```

**The EOI is the correctness point.** `idt.c:700` currently calls
`pic_eoi(r->vector)` unconditionally on the ISA path. Under I/O APIC delivery
that is wrong — EOI must go to the **LAPIC**, or the vector stays in-service and
the second keystroke never arrives. The flag must be set *before* the line is
armed, or the first delivered interrupt EOIs the wrong controller.

**The gate would have to avoid a vacuity trap** of exactly DDR-1040's kind: a
gate asserting "keys still work" **passes with the 8259 still in charge**. It
would need a positive marker naming the GSI actually programmed, a **readback**
of the 8259 mask register (DDR-1046's discipline — "nothing crashed" cannot
distinguish "the mask applied" from "the write never happened"), *and* input
arriving after the cutover.

## 6. Decision

**Deferred, with the blocker named.** Full stage D is blocked on an absent ACPI
`_PRT` parser; the safe subset is buildable but moves two low-rate lines for no
measurable benefit, on the input path every gate depends on, days from a release
whose `v1.0.0` tag is already held. Building a `_PRT` parser is a new ACPI
namespace-walking subsystem — considerably larger than the cutover it would
enable.

**Not claimed:** the I/O APIC is not broken and this is not a defect report.
DDR-874's work stands and is exercised (the table is parsed, overrides are read).
What is deferred is the cutover.

## 7. Files

None — assessment only. The Group A row and
`docs/PRE_LAUNCH_CHECKLIST.md` are updated to record this outcome.
