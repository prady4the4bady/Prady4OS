# DDR-1143 — Installer and persistent root (with the size/risk assessment for DDR-1143..1146)

**Status: DESIGN. Committed before code (NON-NEGOTIABLE 5). No implementation
exists.** This is the parent record for the installer/encryption/recovery track
that PR #17 comment **5839562349** (OWNER-verified, 2026-09-25) put in v1.0.0
scope. That comment supersedes two earlier decisions:

- **#40**, the deferral of full-volume encryption, which I had recommended;
- **#19**, "no installer, stated as a known limitation" (decision in
  5822896320).

The track has four DDRs:

| DDR | Scope |
|---|---|
| **1143** (this) | Installer, disk layout, persistent root, boot-from-disk. Also carries the size/risk assessment and the operator decisions for all four. |
| **1144** | Volume encryption: the crypt block device, header, keyslots, passphrase KDF and unlock. |
| **1145** | TPM 2.0: transport, command subset, measured kernel, sealing and unsealing. |
| **1146** | Per-device recovery key and escrow: what the OS can build, and what must be an external system. |

Every fact below was measured in the tree at `219a943`. Nothing is carried from
memory.

---

## §1 What exists, measured

- **Block layer** (`kernel/drivers/blk/blk.h`):
  - `struct blk_device` is `{read, write, capacity_sectors}` over 512-byte
    sectors.
  - Registered by virtio-blk, AHCI (`ahci.c:291`), NVMe (`nvme.c:507`) and the
    ramdisk (`ramdisk.c:86`).
  - **There is no flush or FUA operation** anywhere in `drivers/blk`, `ahci` or
    `nvme` (grep is empty). So no write is guaranteed durable on hardware with
    a volatile write cache. That is a pre-existing SFS limitation, and a disk
    install makes it real.
  - **There is no partition support.** SFS mounts a whole `blk_device`, and
    `main.c` only reads the MBR signature (`:1307`).
- **SFS** is copy-on-write, and its journal makes only the root swap
  crash-atomic (`sfs.c:27-31`, ADR-018 slice 4g):
  - data blocks are written out of place;
  - the superblock and journal record are overwritten in place.
- **The kernel cannot read its own install media.** `ramdisk.c:5` says so, and
  AHCI explicitly excludes ATAPI (`ahci.c:7`, `:73`). There is also **no USB
  stack** (grep for `xhci|ehci|usb` over `kernel/` is empty).
- **The BIOS disk layout is raw and has no partitions** (`Makefile:753`):
  - LBA 0: stage1 (512 B, 446 B of code, which ends with `times 510-($-$$)`);
  - LBA 1: stage2 (≤ 16 sectors, 1,452 B today);
  - LBA 17: the kernel, up to 48×64 sectors = 1.5 MiB, so it ends by LBA 3089.
- **stage1 already tolerates an MBR partition table.** The ISO's hard-disk
  emulation image is a copy with one written by `mk_hdimg.py`, and it boots in
  `smoke-iso-x86`.
- **The UEFI loader is 6,144 B** (`build/BOOTX64.EFI`) and reads `KERNEL.BIN`
  from its ESP.
  - The ESP is built **host-side** with `mkfs.fat` plus `mtools`
    (`Makefile:1126`).
  - **There is no in-kernel FAT formatter.** The kernel has a FAT32 file
    driver, but no mkfs.
- **The live ISO's root is a RAM disk** (DDR-972), so nothing survives reboot.
  **There are no user accounts and no login.** The OS is single-user by
  decision #25.
- **Headroom:** `kernel.bin` is 1,352,074 B against the 1,572,864 B gate, so
  **220,790 B remain**. Each embedded probe ELF costs a page-aligned 8,192 B.

## §2 The payload problem, and the choice

An installer has to write a bootable system: stage1, stage2, the kernel,
`BOOTX64.EFI`, and an ESP to hold the last two. The running kernel cannot read
the CD it booted from (§1). Three routes:

- **(A) The loaders keep a pristine copy of `kernel.bin` in memory and hand its
  physical address over in `boot_info`.**
  - stage1, stage2 and `BOOTX64.EFI` are small (512 + 1,452 + 6,144 B), so they
    are **embedded in the installer ELF** with `incbin`.
  - The kernel itself cannot be dumped from memory, because `.data` and `.bss`
    are live. So the loader copies it before jumping.
  - Cost:
    - stage2 does a second 48-chunk read to a separate physical range;
    - the UEFI loader does a second `AllocatePages` plus copy;
    - `boot_info` gains two fields. This is **§INV.13's class**: both loaders
      change in one commit.
- **(B) ATAPI in AHCI plus an ISO 9660 reader.** It reads the real media. But
  it is a new command set (PACKET), a new filesystem, and it depends on QEMU's
  CD attachment (IDE on i440fx vs AHCI on q35). Roughly 3× the code of (A).
- **(C) A second, "installer" boot medium.** It moves the problem rather than
  solving it.

**Choice: (A).** It is mine to make and needs no operator decision. It also
gives the installer a known-good kernel image independent of the boot medium.
That property matters: a USB-stick boot would have the same "cannot read the
media" problem, because there is no USB stack.

## §3 Installed disk layout

The disk uses **MBR**, not GPT. GPT's header lives at LBA 1, which is exactly
where stage2 sits, so a GPT disk would need stage1/stage2 relocated and both
loaders re-proven. UEFI firmware boots an MBR disk's type-`0xEF` partition, and
OVMF does. That is a checked claim, to be **measured** in gate arm U, not
assumed.

| Region | LBA | Contents |
|---|---|---|
| Boot area | 0 | stage1 plus an MBR table with two entries |
| | 1..16 | stage2 |
| | 17..3089 | `kernel.bin` (raw, for the BIOS path) |
| gap | ..4095 | zero |
| P1 ESP, type `0xEF` | 4096, 64 MiB | FAT16: `EFI/BOOT/BOOTX64.EFI` and `KERNEL.BIN` |
| P2 root, type `0xDA` (non-FS data) | P1 end .. disk end | a DDR-1144 encrypted volume containing SFS |

Notes on the layout:

- **Both loaders carry the same kernel bytes.** The kernel is written twice:
  raw at LBA 17 for stage2, and as `KERNEL.BIN` for the UEFI loader.
  "Byte-identical, nothing conditional on which loader ran" (`Makefile:1127`)
  stays true.
- **FAT16, not FAT32**, for a small ESP. UEFI accepts FAT12/16/32 on an ESP.
  The installer carries a **minimal FAT16 formatter** that writes one boot
  sector, two FATs and a root directory, then lays out two files contiguously.
  That is about 250 lines. A cheaper host-built "skeleton" image was
  considered and **refused**: its cluster positions would be a silent contract
  with a host tool.
- The partition type byte `0xDA` is chosen so that no other OS auto-mounts P2
  as a filesystem it knows.

## §4 Components

1. **Partition sub-device.**
   - `blk_part_create(parent, lba_start, sectors)` registers a `blk_device`
     whose read/write add an offset and **bounds-check** against the partition
     end.
   - SFS then mounts a partition unchanged.
   - Bounds: an out-of-range LBA returns `-EINVAL`. It is never clamped or
     wrapped.
2. **Flush.**
   - A `flush` op on `blk_device`, implemented in three places:
     - virtio-blk `VIRTIO_BLK_T_FLUSH`, when `VIRTIO_BLK_F_FLUSH` is negotiated;
     - AHCI `FLUSH CACHE EXT` (0xEA);
     - NVMe Flush (opcode 0x00).
   - The ramdisk no-ops.
   - SFS `txn_commit` gains flush barriers: data blocks, then flush, then the
     journal record, then flush, then the superblock.
   - That makes the existing root-swap atomicity true **on a disk with a write
     cache**. Today it is true only on QEMU's defaults.
3. **Disk enumeration** as a syscall, listing name, capacity, and whether the
   disk is the boot medium or a ramdisk. It is **NSI 104**, the next free
   (verified in `syscall.h:185`).
4. **The install syscall(s)**, sovereign-only. Ring 3 must never be able to
   write the boot area of an arbitrary disk without this gate. It is a
   capability check in the S2 invariant family.
5. **The `install` ring-3 program**, a text flow on the PRISM console:
   - list the disks, choose one, and type the disk name to confirm the wipe;
   - set the passphrase (DDR-1144);
   - show the recovery key and write the escrow record (DDR-1146);
   - write, verify by reading back, and reboot.
   - **Whole-disk wipe only.** No resizing and no dual-boot.
6. **Installed-boot root selection.**
   - At boot the kernel scans disks for this layout's MBR signature plus the
     DDR-1144 volume header magic in P2. If found, it unlocks (DDR-1144/1145)
     and mounts SFS from the crypt device as the **default root**, instead of
     the DDR-972 ramdisk.
   - If nothing is found, the live path is **unchanged**. That is the
     compatibility claim, and every one of the 182 existing gates tests it for
     free.

**User accounts and login are NOT included, and they are not cheap.** No
concept of a user exists anywhere in the tree. So they are named as a
**follow-on**. This bears directly on DDR-1145 §5: without a login, TPM-only
auto-unlock protects less than it appears to.

## §5 Gate design (installer half); vacuity checked before writing

**`smoke-install`** runs QEMU with a **blank** virtio disk attached beside the
ISO:

- **Boot 1:** the ISO boots, the injector drives `install`, and the setup
  writes a marker file `/INSTALLED.MARK`, whose content is a random nonce
  **generated by the guest** and printed.
- **Boot 2:** the ISO is **removed**, and QEMU boots from the disk alone, once
  under BIOS and once under OVMF.

Arms:

- **P:** boot 2 prints the **same nonce** read back from `/INSTALLED.MARK`.
  - The vacuous version would assert only that "boot 2 reached
    `NEXUS KERNEL OK`". A disk that boots but roots at a RAM disk passes that.
    The nonce is the one thing a RAM-disk root cannot produce, because it was
    generated in boot 1.
- **B / U:** boot 2 succeeds on both loaders, with `[uefi] handoff` present on
  the U arm only.
- **R:** boot 2 prints `[root] disk` and **not** `[ramdisk] formatted SFS`.
- **Negative:** a disk carrying a foreign MBR and no volume magic boots the
  **live** path. Detection must not take over a disk it did not write.

Planned mutants:

| Mutant | Change | Must fail |
|---|---|---|
| M1 | the installer skips the SFS format | arm P |
| M2 | the partition sub-device drops the offset | arms P and R |
| M3 | the UEFI kernel copy is written but the ESP FAT entry is wrong | arm U only |
| M4 | root selection ignores the header | arm R |

**Crash consistency is recorded, not claimed.** The flush barriers (§4.2) are
testable only for ordering, by trace. QEMU cannot pull power mid-write. So "the
disk survives power loss" is a **hardware-only** claim (§7).

## §6 Size and risk: all four DDRs

These are estimates, stated as ranges. None is a measurement.

| Piece | New code (approx.) | New gates | Risk | Why |
|---|---|---|---|---|
| DDR-1143: partition, flush, installer, FAT16, root selection, loader pristine copy | 1,800–2,400 lines | `smoke-install` (two boots × two loaders) | **High** | It changes **both boot loaders**. §INV.13/§INV.18 class, and the loaders are the least-instrumented code in the tree. |
| DDR-1144: crypt device, header, keyslots, KDF, unlock syscall and prompt | 1,200–1,600 | `smoke-crypt` | Medium | The crypto is reused. The risk is in the construction (DDR-1144 §2) and in wiring unlock ahead of root mount. |
| DDR-1145: TPM TIS+CRB, TPM2 command subset, loader kernel measurement, seal/unseal/reseal | 1,800–2,500 | `smoke-tpm` (needs swtpm in CI) | **High** | A new subsystem. Real-chip behaviour is **not verifiable here** (DDR-1145 §6). |
| DDR-1146: recovery key, device id, escrow record, export, rotation after use; host reference tool | 500–800 in the OS, plus a ~300-line host tool | `smoke-recovery` | Medium in the OS | The **backend is external and is not built** (DDR-1146 §4). |
| **Total** | **~5,300–7,300 lines** | 4 gates, some multi-boot | | |

- **Kernel headroom:** 220,790 B is enough for the kernel-side pieces, roughly
  60–110 KB, if the installer, unlock and recovery programs are ring-3 ELFs
  that are **not** embedded in `kernel.bin`, as `term.elf` is not today. If it
  is not enough, the stage2 window must be raised in both loaders (§INV.18).
  That would be flagged at the time, not absorbed silently.
- **Time:** at the cadence of DDR-1139..1142, including CI three-green cycles,
  roughly **8–12 working sessions**. The TPM half is the largest single
  uncertainty.
- **Sequencing:** 1143 → 1144 → 1146 (device side) → 1145.
  - TPM goes last because it is the most separable and the riskiest.
  - Passphrase plus recovery is a complete, testable encryption story without
    it.
  - Your instruction requires **both layers present** before v1.0.0, so 1145 is
    not optional. It is only last.

## §7 What cannot be verified without physical hardware

This is stated plainly, as the operator asked.

1. **Power-loss durability** of the flush ordering on a real disk with a write
   cache.
2. **Real UEFI firmware** booting an MBR ESP. OVMF is the proxy, exactly as for
   DDR-1142.
3. **Real NVMe/SATA controllers** under an install-sized write load. Only QEMU
   models are exercised.
4. **Everything TPM-specific that swtpm does not model:**
   - a discrete TPM's bus exposure;
   - fTPM/CRB vendor quirks;
   - real firmware PCR values and event logs;
   - dictionary-attack lockout timing.
   See DDR-1145 §6.

## §8 Decisions needed from the operator (all four DDRs)

| # | Decision | Where argued | My recommendation |
|---|---|---|---|
| D1 | Escrow key custody. **Any escrow that can recover every device holds, by construction, a credential that decrypts every record.** Options: one HSM key; threshold M-of-N; per-distributor keys; owner-bound shares. | DDR-1146 §3 | Threshold (2-of-3) in HSMs, with per-distributor records. |
| D2 | How the escrow record leaves the device: printed/typed text (needs nothing new) vs network upload (needs your escrow endpoint and its public key). | DDR-1146 §2.1 | Text record for v1. Upload after a backend exists. |
| D3 | Where the escrow backend lives, who runs the offices' identity check, and where the office-side audit log is kept. **External to this OS.** | DDR-1146 §4 | Yours. The OS side is built against a record format, not a service. |
| D4 | TPM-only auto-unlock vs **TPM+PIN**, given there are no user accounts. | DDR-1145 §5 | **TPM+PIN**, or TPM-only with the limitation stated in the release notes. |
| D5 | Passphrase KDF: PBKDF2-HMAC-SHA256 (composes existing primitives; not memory-hard) vs Argon2id (needs BLAKE2b, a **new** primitive, KAT-gated like ML-DSA). | DDR-1144 §4 | Argon2id if the schedule allows. Otherwise PBKDF2 with the weakness stated. |
| D6 | BIOS-booted installs get **passphrase only** (no TPM measurement path on the BIOS side). | DDR-1145 §4 | Accept. |

### §8.1 Decided 2026-09-26 (PR #17 comment 5841525203, OWNER)

| # | Decision |
|---|---|
| D1 | Threshold 2-of-3 custody plus per-distributor escrow keys (DDR-1146 §3 (b)+(c)). Device side: one `EPK` per build. |
| D2 | Text escrow record for v1 (`ESCROW.TXT` on the ESP, DDR-1146 §2.1). |
| D3 | The escrow backend, the office identity/ownership check and the office audit log are the operator's, external. The OS builds against the record format only. |
| D4 | **TPM+PIN.** TPM-only is rejected: with no login, a stolen machine must not reach the desktop on power-on alone. |
| D5 | **Argon2id**, if the schedule allows. If it does not fit, fall back to PBKDF2 **and say so explicitly with the reason**; no silent downgrade. |
| D6 | BIOS-booted installs get passphrase + recovery only, no TPM. |

All four DDRs are approved to proceed in the order 1143 -> 1144 -> 1146 -> 1145.
The first TPM-path step, measuring OVMF's TCG2 support, is done: DDR-1145 §1.1,
and the track proceeds.

## §9 Not claimed

- **No implementation exists.** This is a design.
- **Nothing is built, gated or tagged.**
- The line-count and session estimates are **estimates**.
- **No physical-hardware claim** of any kind.
- "No installer, a known limitation" (#19) and the #40 deferral are
  **superseded by operator instruction 5839562349**, not by anything built.
- Kernel unchanged: `9ff230a9dc3395ec`, 1,352,074 B. GLOBAL_FORBIDDEN 77. 182
  gates.
