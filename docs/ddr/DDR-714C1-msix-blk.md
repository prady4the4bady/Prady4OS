# DDR-714 stage C1 — MSI-X for virtio-blk (IRQ routing off the 8259)

> DDR before code. Stage C moves device IRQs off the legacy 8259/INTx plumbing.
> C1 starts with MSI-X — NOT the I/O APIC — deliberately: on q35 the PCI INTx →
> I/O APIC GSI mapping does not match the PIC-mode `Interrupt Line` values the
> drivers read (SeaBIOS programs those for 8259 routing), and rather than guess
> the swizzle, MSI-X bypasses the question entirely: the device writes its
> interrupt message straight to the LAPIC. This also unshares the virtio INTx
> lines (blk0/1 + net share IRQ 11; blk2/3 share 10 — the chained-handler
> workaround in idt.c) and is the prerequisite for multi-in-flight block I/O.

## Decisions
- **D1 — scope: virtio-blk only.** 4 disks → per-device MSI-X vectors 50..53
  (the IDT grows 50→54 stubs). net/input/gpu stay on INTx this slice (mixed
  mode is per-device); they migrate in C2.
- **D2 — transport helper `virtio_pci_msix_setup(d, vector)`.** Walk the PCI
  capability list for cap ID 0x11; map the table BAR (existing `map_bar`);
  program table entry 0 = {addr `0xFEE00000 | bsp_apic_id<<12`, data `vector`,
  unmasked}; set message-control MSI-X-enable; set the command register's INTx
  disable bit for this function. In the virtio common config: `msix_config =
  0xFFFF` (no config-change vector) and, with the queue selected,
  `queue_msix_vector = 0` (table entry 0) — verified by reading it back
  (0xFFFF read-back = the device rejected it). Returns -1 on any absence so the
  caller falls back to INTx (no MADT/IOAPIC dependency at all).
- **D3 — dispatch.** `msix_register(vector, fn)` in idt.c (mirror of
  `irq_register`); `isr_dispatch` routes vectors 50..53 to the registered
  handler + `lapic_eoi` (MSI is edge — LAPIC EOI only, never the PIC). The
  MSI-X completion handler skips the INTx `isr_ack` read (that register is the
  INTx level deassert; MSI-X does not use it) and completes just its own device
  (no more poll-all-sharers).
- **D4 — destination = the BSP for now.** All virtio-blk completion state
  (`done`/`waiter`) currently assumes the IRQ fires where the old INTx did
  (BSP). Distributing device vectors across APs is C2/C3, WITH the locks-2 D2
  completion-field review it requires.

## Gate
None new: `smoke-fs`/`-rw`/`-sfs-rw`/`-ext4`/`smoke-user` all ride virtio-blk
completions every run — if MSI-X misfires nothing boots past the FS phase. The
serial line `virtio-blk: blkN ... msix vec=V` (vs `IRQ n`) shows the path taken.
61 gates.

## Non-goals
net/input MSI-X (C2); routing vectors to APs + completion-field audit (C2/C3);
I/O APIC for the ISA lines — keyboard/COM1 stay on the 8259 (C2); multi-queue /
multi-in-flight requests (needs per-request tags; enabled by this but separate).
