# Phase 3 — Driver Framework — Test Results

## Slice 3a: ACPI table parser + PCIe MMCONFIG enumeration

- **Date:** 2026-06-18
- **Decision:** ADR-013.
- **Files:** `kernel/acpi/acpi.{c,h}`, `kernel/drivers/pcie/pcie.{c,h}`,
  `kernel/mm/vmm.h` (VMM_PCD/PWT), `kernel/main.c`, `tools/qemu_runner/boot_test.sh`
  (q35 + virtio-blk boot + virtio-net).

### Commands

```bash
make image && make smoke    # boots on q35 from virtio-blk; smoke PASS
```

### Verified (QEMU q35)

```
ACPI: RSDT, 5 tables (rev 0)
PCIe: ECAM 0x00000000B0000000 (bus 0+) — scanning bus 0
  0:0.0   0x8086:0x29C0  class 0x06/0x00  host bridge
  0:1.0   0x1234:0x1111  class 0x03/0x00  display controller
  0:2.0   0x1AF4:0x1001  class 0x01/0x00  storage controller   (virtio-blk)
  0:3.0   0x1AF4:0x1000  class 0x02/0x00  network controller   (virtio-net)
  0:31.0  0x8086:0x2918  class 0x06/0x01  ISA bridge
  0:31.2  0x8086:0x2922  class 0x01/0x06  SATA controller
  0:31.3  0x8086:0x2930  class 0x0C/0x05  serial bus controller
PCIe: 7 devices enumerated
```

- The ACPI RSDP was found in the BIOS area and its RSDT walked; the MCFG table
  gave the ECAM base (0xB0000000), which was mapped uncached and scanned.
- All three target devices appear: **virtio-blk** (1AF4:1001), **virtio-net**
  (1AF4:1000), and the **VGA display controller** (1234:1111).
- Booting from a virtio-blk disk works (SeaBIOS INT 13h; Stage 1 unchanged).
- smoke PASS; warning-free `-Werror` build.

### Not done yet

- Multi-bus / bridge-recursive scanning (only bus 0 mapped/scanned).
- BAR programming, MSI/MSI-X (need APIC).
- Per-table ACPI checksum validation (RSDP checksum is validated).
- A real device *driver* on top (virtio / NVMe / framebuffer) — next slice.
- MADT (APIC) and FADT (power) parsing — the table parser is ready for them.
