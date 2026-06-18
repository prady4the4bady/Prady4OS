# ADR-013: ACPI table discovery + PCIe MMCONFIG enumeration

- **Status:** Accepted 2026-06-18
- **Phase:** 3 (slice 3a)

## Context

Layer 3 (drivers) needs to discover hardware. The foundation is PCIe
enumeration, which on a PCIe machine uses MMCONFIG/ECAM, whose base comes from
the ACPI MCFG table. So an ACPI table parser is the real first step — and it
also unblocks MADT (APIC) and FADT (power management) later.

## Decision

- **QEMU machine: q35.** The default `pc` (i440FX) machine has no MMCONFIG/MCFG.
  q35 is a PCIe machine with an MCFG table. The runner (`boot_test.sh`) now uses
  `-machine q35`, boots from a **virtio-blk** disk (`bootindex=0`, SeaBIOS boots
  it via INT 13h — Stage 1 is unchanged), and adds a **virtio-net** device so the
  scan has real devices to find.
- **ACPI parser** (`kernel/acpi/`): scan 0xE0000–0xFFFFF for the RSDP (checksum-
  validated), then walk the RSDT (or XSDT on ACPI 2.0+) and return tables by
  4-char signature (`acpi_find_table`). Tables are in identity-mapped low RAM.
- **PCIe** (`kernel/drivers/pcie/`): read MCFG for the ECAM base, map bus 0's
  1 MiB of config space **uncached** (`VMM_PCD`) into a kernel VA window
  (0xFFFFC00000000000), and scan bus 0 — `pcie_read32` addresses config space as
  `base + (bus<<20 | dev<<15 | func<<12 | off)`. Multi-function devices are
  probed via header-type bit 7. Results go into a small device registry.

## Consequences / deferred

- Only bus 0 is mapped/scanned (QEMU q35 puts devices there). Bridge-recursive
  multi-bus scanning + mapping more ECAM is a later extension.
- ECAM mapped RW+PCD (uncached); fine for config space.
- No MSI/MSI-X or BAR programming yet — that comes with individual device
  drivers (NVMe/virtio) and the APIC.
- The ACPI parser validates the RSDP checksum; per-table checksum validation is
  a small future hardening.

## Verification

QEMU q35: ACPI RSDT found (5 tables); MCFG → ECAM 0xB0000000; bus-0 scan
enumerates 7 devices including virtio-blk (1AF4:1001), virtio-net (1AF4:1000),
and the VGA display controller (1234:1111). Boot-from-virtio-blk works; smoke
PASS; warning-free `-Werror` build.
