# DDR-978 — Full manual ISO verification: BIOS passes, **UEFI has no PCIe at all**

Status: ACCEPTED — verification record + a new defect found by it, with the fix.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

**Supersedes the failing verdict of:** DDR-971 (which found the ISO booted a
kernel and no OS). That defect is fixed (DDR-972) and this DDR re-runs the full
walkthrough on the fixed image.

Artefact: `build/pradyos.iso`, **52,805,632 B**,
`sha256 5a966d190df0c34f755a6be421e1b80e…`. Kernel `1,061,246 B`.

---

## 1. Method

Not a sentinel grep. A FIFO drives the guest's real serial console once
`PRISM_READY` appears and issues the shell session a person would:
`uname, ls, ps, free, uptime, date, echo, touch, echo>file, cat, ls, mv, cat,
rm, ls, dmesg, agent, mode`. Both firmware paths, same script:
`build/gatelogs/isowalk_seabios.log` (641 lines) and `isowalk_uefi.log` (563).

## 2. SeaBIOS arm — PASS, and the checks are state-changing

| check | evidence |
|---|---|
| boots an OS | `NEXUS KERNEL OK`, `[fs] mounted`, `PRISM_READY`, no `PANIC` |
| DDR-972 ramdisk root | `ramdisk` ×4, `formatted SFS — ISO root ready` |
| `uname` | `AuthenticAMD "QEMU Virtual CPU version 2.5+" cpus=1` — real CPUID |
| `ps` | a real table: PID/PPID/state/user-kernel/ticks/name, incl. `AETHERD` (pid 42, ppid 40, `R u`) |
| `free` | `mem: total=114556K free=97848K used=16708K` |
| `uptime` / `date` | `uptime: 7s`, `date: 2026-08-22 20:06:33` |
| **file lifecycle** | `ls`→`KOUT.TXT` → `touch: /WALK.TXT` → `ls`→`KOUT.TXT`+**`WALK.TXT`** → `mv: /WALK.TXT -> /WALK2.TXT` → `cat`→`walk-content-4c9` → `rm: removed /WALK2.TXT` → `ls`→`KOUT.TXT` |
| `dmesg` | `dmesg: 4096 bytes`, then a ring dump starting mid-word (`minating`) |
| AETHER | `PRADYOS_AGENTS_OK`, `AGENT KRYOS active` + 5 inactive; `PRADYOS_AETHER_DAEMON_OK mode=sovereign` |
| GPU | `PRADYOS_GPU_FB_OK 1024x768 BGRA scanout0`, `[gpu] page-flip OK` |
| networking | `[net] virtio-net up MAC=…`, `[net] lwIP up 10.0.2.15/24`, `PRADYOS_NET_LO_OK` |
| compositor | `PRADYOS_SURFACE_CLIENT_OK`, `PRADYOS_BACKDROP DAWN/DAY/DUSK`, `PRADYOS_AMBIANCE_OK` |

The file-lifecycle row is the one that matters: `ls` **changes** across
touch/mv/rm. A stubbed `ls` cannot produce that sequence.

**An artefact I had to explain before trusting the log.** A 100-line block
appears twice, at a constant offset, in a capture with `NEXUS KERNEL OK` ×1 (so
only one boot). It is not a double execution and not a reboot: `dmesg: 4096
bytes` immediately precedes it and the block begins **mid-word**, because
`dmesg` dumps the 4 KiB kernel log ring, which contains the console history the
session just produced. The duplication *is* `dmesg` working.

## 3. UEFI arm — boots, runs PRISM, and has **zero PCI devices**

The walkthrough completes (`[uefi] handoff`, `NEXUS KERNEL OK`, `PRISM_READY`,
`iso-walk-end-3f1`, no panic, 235 s of heartbeats — not truncated). But:

```text
SeaBIOS: ACPI: RSDT, 5 tables (rev 0)      UEFI: ACPI: RSDP not found
         ACPI: FADT ok, S5 found                 ACPI: no FADT (no S5)
         PCIe: ECAM 0xB0000000 — scanning        PCIe: no MCFG table
         PCIe: 7 devices enumerated              (nothing enumerated)
         PRADYOS_GPU_FB_OK  ✓                    PRADYOS_GPU_FB_OK  ABSENT
         PRADYOS_NET_LO_OK  ✓                    PRADYOS_NET_LO_OK  ABSENT
```

`NEXUS: boot_info OK vendor=` is also **empty** under UEFI (`AuthenticAMD` under
BIOS).

### 3.1 Root cause

`kernel/acpi/acpi.c:31` — the RSDP is found by **one** method:

```c
/* Scan the BIOS area for the RSDP (QEMU/SeaBIOS place it in 0xE0000..0xFFFFF). */
for (uint64_t a = 0xE0000; a < 0x100000; a += 16) …
```

UEFI firmware is under no obligation to place the RSDP in that legacy window,
and OVMF does not. It publishes it through the **EFI Configuration Table**.
`boot/uefi/loader.c` never reads that table and never passes the pointer on, so
the kernel's only discovery path finds nothing. The cascade is total:

`no RSDP` → no XSDT/RSDT → **no MCFG** (no PCIe ECAM → *no devices at all*),
**no MADT** (→ no AP enumeration, so UEFI boots are uniprocessor by accident),
**no FADT** (→ no ACPI S5 poweroff, no reset register).

The ISO boots under UEFI **only** because DDR-972's ramdisk fallback triggers on
`blk_count() == 0` — the very condition this defect creates. The fix for one
release-blocker is masking another.

### 3.2 Why `smoke-uefi` is green anyway

```make
EXTRA_SENTINEL="[uefi] handoff\nNEXUS: E820 map, entries=0x0000000000000010"
FORBIDDEN_SENTINEL="[uefi] FATAL"
```

Both assertions are true of a machine with no PCIe. The gate proves the *loader
handoff*, which is what DDR-886 built it for — the same vacuity class as DDR-971
(`smoke-iso-x86` asserting `NEXUS KERNEL OK` at line 30 of 145), DDR-973 (a
period-256 pattern), and DDR-880's harness-echo detector. **Fourth time.**

## 4. The fix

**Pass the RSDP through the existing spare field.** `struct boot_info` ends in a
flexible array, so nothing may be appended (`boot_info.h:25`) — but it already
carries `uint32_t reserved` at offset 28, and `stage2.asm:108-115` **zeroes the
whole 32-byte header** before stamping the magic. So:

- rename `reserved` → `acpi_rsdp` (same offset, same size, header stays 32 B and
  both `_Static_assert`s in `loader.c` still hold);
- **BIOS path unchanged**: stage2 leaves it 0, kernel falls back to the legacy
  scan exactly as today;
- **UEFI path**: the loader walks `SystemTable->ConfigurationTable` for the ACPI
  2.0 GUID (falling back to ACPI 1.0) and stores the address;
- the kernel **validates** the handed pointer (`"RSD PTR "` + checksum) before
  trusting it and falls back to the scan if it fails, so a garbage or truncated
  value degrades to today's behaviour rather than following a wild pointer.

32 bits is sufficient and is checked, not assumed: firmware ACPI tables sit well
below 4 GiB on x86_64, and the loader refuses to store an address ≥ 4 GiB (it
leaves 0, and the kernel says so) rather than silently truncating.

`boot/uefi/efi.h`'s `EFI_SYSTEM_TABLE` stops at `boot_services`; the two spec
fields after it (`NumberOfTableEntries`, `ConfigurationTable`) are appended.
That is a read-only extension into a table the firmware already provides.

## 5. Gate — the vacuity is the real defect

`smoke-uefi` gains assertions that a deviceless boot cannot satisfy:
`ACPI: ` root table found, `PCIe: ECAM`, and `[net] virtio-net up`. A UEFI boot
that enumerates nothing must fail the gate, not pass it.

## 5.1 Fix verified — measured, both arms

**UEFI ESP boot, before → after:**

```text
before: NEXUS: boot_info OK  vendor=            <- empty
        ACPI: RSDP not found
        ACPI: no FADT (no S5)
        PCIe: no MCFG table (need a PCIe machine, e.g. QEMU q35)
        (0 devices)

after:  NEXUS: boot_info OK  vendor=AuthenticAMD
        ACPI: RSDP from loader
        ACPI: XSDT, 6 tables (rev 2)            <- XSDT, i.e. the ACPI 2.0 RSDP
        ACPI: FADT ok, S5 found, reset-reg      <- poweroff/reset now available
        PCIe: ECAM 0x00000000E0000000 (bus 0+) — scanning bus 0
        PCIe: 10 devices enumerated
```

**Full ISO walkthrough under OVMF, after the fix** (`isowalk_uefi.log`, 639
lines) — every item that was absent in §3 is now present:

| check | before | after |
|---|---|---|
| `PCIe: … devices enumerated` | **0** | **7** |
| `[fs] mounted` | ✓ (ramdisk fallback only) | ✓ |
| `PRISM_READY` | ✓ | ✓ |
| `PRADYOS_GPU_FB_OK` | **absent** | ✓ |
| `PRADYOS_NET_LO_OK` | **absent** | ✓ |
| `PRADYOS_SURFACE_CLIENT_OK` | **absent** | ✓ |
| `PANIC` | 0 | 0 |

The UEFI arm now matches the SeaBIOS arm.

## 5.2 The hardened gate is not vacuous — mutation-checked

`smoke-uefi` gains `ACPI: RSDP from loader`, `ACPI: FADT ok` and `PCIe: ECAM` as
required sentinels, and — the discriminating half — `ACPI: RSDP not found`,
`PCIe: no MCFG table` and `ACPI: loader RSDP rejected` as **forbidden** ones.
Each forbidden string is a literal line the broken path printed.

Mutant: restore the pre-fix behaviour (`bi->acpi_rsdp = 0`) in the loader.

```text
mutant applied  -> make: *** [Makefile:1092: smoke-uefi] Error 1
mutant reverted -> [smoke] PASS — saw 'NEXUS KERNEL OK' + 5 FS pattern(s)
```

The gate now fails on exactly the defect it slept through.

## 6. Verdict

- **SeaBIOS: PASS.** Every STEP-1 item verified against observed behaviour.
- **UEFI: was FAIL, root-caused, FIXED and re-verified** (§5.1) — 0 → 7 PCI
  devices, GPU + networking + compositor all present, gate mutation-checked
  (§5.2).
- **`v1.0.0` still not tagged here.** Both firmware paths now pass the manual
  walkthrough, which is the gate the operator set for tagging — but `main` is at
  `7c6c67a` and carries *neither* DDR-972's ramdisk root nor this fix. Tagging
  `main` today would tag the DDR-971 image. The remaining sequence is: merge the
  branch, re-run this walkthrough on `main`'s own ISO, then tag.
